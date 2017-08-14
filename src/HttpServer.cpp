#include "HttpServer.h"

#include <libffmpegthumbnailer/videothumbnailer.h>
#include <condition_variable>
#include <fstream>
#include <queue>
#include <sstream>

#include "CurlHttp.h"
#include "Utility.h"

using namespace std::string_literals;

const std::string SEPARATOR = "--";

namespace {

template <class Request>
Json::Value finalize_request(Request request) {
  if (!request->provider()) {
    Json::Value result;
    result["error"] = "invalid provider"s;
    return result;
  }

  return request->result();
}

std::string file_type_to_string(IItem::FileType type) {
  switch (type) {
    case IItem::FileType::Audio:
      return "audio";
    case IItem::FileType::Directory:
      return "directory";
    case IItem::FileType::Image:
      return "image";
    case IItem::FileType::Unknown:
      return "unknown";
    case IItem::FileType::Video:
      return "video";
    default:
      return "invalid";
  }
}

Json::Value tokens(ICloudProvider::Pointer p) {
  Json::Value result;
  result["token"] = p->token();
  auto hints = p->hints();
  auto it = hints.find("access_token");
  if (it != std::end(hints)) result["access_token"] = it->second;
  result["provider"] = p->name();
  return result;
}

class ResponseCallback : public IHttpServer::IResponse::ICallback {
 public:
  ResponseCallback(std::shared_ptr<std::stringstream> p) : result_(p) {}

  int putData(char* buffer, size_t size) override {
    auto r = result_->readsome(buffer, size);
    if (r == 0)
      return -1;
    else
      return r;
  }

 private:
  std::shared_ptr<std::stringstream> result_;
};

}  // namespace

CloudConfig::CloudConfig(const Json::Value& config,
                         MicroHttpdServerFactory::Pointer f)
    : auth_url_(config["auth_url"].asString()),
      file_url_(config["file_url"].asString()),
      daemon_port_(config["daemon_port"].asInt()),
      youtube_dl_url_(config["youtube_dl_url"].asString()),
      keys_(config["keys"]),
      file_daemon_(DispatchServer(f, daemon_port_)) {}

std::unique_ptr<ICloudProvider::Hints> CloudConfig::hints(
    const std::string& provider) const {
  if (!keys_.isMember(provider)) return nullptr;
  auto p = std::make_unique<ICloudProvider::Hints>();
  auto& hints = *p;
  hints["youtube_dl_url"] = youtube_dl_url_;
  hints["client_id"] = keys_[provider]["client_id"].asString();
  hints["client_secret"] = keys_[provider]["client_secret"].asString();
  hints["redirect_uri"] = auth_url_;
  hints["file_url"] = file_url_;
  return p;
}

IHttpServer::IResponse::Pointer
HttpServer::ConnectionCallback::receivedConnection(
    const IHttpServer& d, IHttpServer::IConnection::Pointer c) {
  util::log("got request", c->url());
  Json::Value result;
  const char* key = c->getParameter("key");
  if (!key) {
    result["error"] = "missing key";
  } else {
    const char* provider = c->getParameter("provider");
    if (provider) {
      auto p = server_->provider(key + SEPARATOR + provider);
      auto r = p->provider(*c);
      if (!r) {
        result["error"] = "invalid provider";
      } else {
        auto stream = std::make_shared<std::stringstream>();
        auto cb = std::make_unique<ResponseCallback>(stream);
        auto func = [=](auto e) {
          *stream << Json::StyledWriter().write(e);
          c->resume();
        };
        c->suspend();
        if (c->url() == "/exchange_code"s) {
          p->exchange_code(r, *c, func);
        } else if (c->url() == "/list_directory"s) {
          p->list_directory(r, *c, func);
        } else if (c->url() == "/get_item_data"s) {
          p->get_item_data(r, *c, func);
        } else if (c->url() == "/thumbnail"s) {
          p->thumbnail(r, *c, func);
        }
        return d.createResponse(IHttpRequest::Ok,
                                {{"Content-Type", "application/json"}}, -1,
                                1024, std::move(cb));
      }
    } else {
      if (c->url() == "/list_providers"s)
        result = server_->list_providers(*c);
      else
        result["error"] = "invalid request";
    }
  }

  auto str = Json::StyledWriter().write(result);
  return d.createResponse(200, {{"Content-Type", "application/json"}}, str);
}

HttpServer::HttpServer(Json::Value config)
    : server_factory_(std::make_unique<MicroHttpdServerFactory>(
          util::read_file(config["ssl_cert"].asString()),
          util::read_file(config["ssl_key"].asString()))),
      main_server_(server_factory_->create(
          std::make_unique<ConnectionCallback>(this), "",
          MicroHttpdServer::Type::SingleThreaded, config["port"].asInt())),
      config_(config, server_factory_) {}

ICloudProvider::IAuthCallback::Status
HttpServer::AuthCallback::userConsentRequired(const ICloudProvider& p) {
  util::log("waiting for user consent", p.name());
  return Status::None;
}

void HttpServer::AuthCallback::done(const ICloudProvider& p,
                                    EitherError<void> e) {
  if (e.left()) {
    data_->set_status(HttpCloudProvider::Status::Denied);
    util::log("auth error", e.left()->code_, e.left()->description_);
  } else {
    data_->set_status(HttpCloudProvider::Status::Accepted);
    util::log("accepted", p.name(), p.token());
  }
}

ICloudProvider::Pointer HttpCloudProvider::provider(
    const IHttpServer::IConnection& connection) {
  const char* provider = connection.getParameter("provider");
  const char* token = connection.getParameter("token");
  if (!provider) return nullptr;
  std::lock_guard<std::mutex> lock(lock_);
  if (!provider_ || status_ == Status::Denied) {
    provider_ = ICloudStorage::create()->provider(provider);
    if (!provider_) return nullptr;
    ICloudProvider::InitData data;
    if (token) data.token_ = token;
    data.http_server_ =
        std::make_unique<ServerWrapperFactory>(config_.file_daemon_);
    data.http_engine_ = std::make_unique<CurlHttp>();
    const char* access_token = connection.getParameter("access_token");
    data.hints_ = *config_.hints(provider);
    if (access_token) data.hints_["access_token"] = access_token;
    data.hints_["state"] = key_;
    data.callback_ = std::make_unique<HttpServer::AuthCallback>(this);
    provider_->initialize(std::move(data));
  }
  return provider_;
}

void HttpCloudProvider::exchange_code(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection,
    Completed c) {
  const char* code = connection.getParameter("code");
  if (!code) {
    Json::Value result;
    result["error"] = "missing code";
    return c(result);
  }
  p->exchangeCodeAsync(code, [=](auto token) {
    if (token.right()) {
      Json::Value result;
      result["token"] = *token.right();
      result["provider"] = p->name();
      c(result);
    } else {
      c(error(p, *token.left()));
    }
  });
}

void HttpCloudProvider::list_directory(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection,
    Completed c) {
  item(p, connection, [=](auto item) {
    if (item.left()) return c(error(p, *item.left()));
    p->listDirectoryAsync(item.right(), [=](auto list) {
      if (list.right()) {
        Json::Value result = tokens(p);
        Json::Value array(Json::arrayValue);
        for (auto i : *list.right()) {
          Json::Value v;
          v["id"] = i->id();
          v["filename"] = i->filename();
          v["type"] = file_type_to_string(i->type());
          array.append(v);
        }
        result["items"] = array;
        c(result);
      } else {
        c(error(p, *list.left()));
      }
    });
  });
}

void HttpCloudProvider::get_item_data(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection,
    Completed c) {
  item(p, connection, [=](auto item) {
    if (item.left()) {
      c(error(p, *item.left()));
    } else {
      Json::Value result = tokens(p);
      result["url"] = item.right()->url();
      result["id"] = item.right()->id();
      c(result);
    }
  });
}

void HttpCloudProvider::item(ICloudProvider::Pointer p,
                             const IHttpServer::IConnection& connection,
                             CompletedItem c) {
  const char* item_id = connection.getParameter("item_id");
  if (!item_id)
    c(Error{IHttpRequest::NotFound, "not found"});
  else
    item_id == "root"s ? c(p->rootDirectory())
                       : (void)p->getItemDataAsync(item_id, c);
}

void HttpCloudProvider::thumbnail(ICloudProvider::Pointer p,
                                  const IHttpServer::IConnection& connection,
                                  Completed c) {
  item(p, connection, [=](auto item) {
    if (item.left()) return c(error(p, *item.left()));

    class download : public IDownloadFileCallback {
     public:
      download(EitherError<IItem> item, ICloudProvider::Pointer p, Completed c)
          : item_(item), p_(p), c_(c) {}

      void receivedData(const char* data, uint32_t length) override {
        for (size_t i = 0; i < length; i++) data_.push_back(data[i]);
      }
      void done(EitherError<void> thumbnail) override {
        auto i = item_.right();
        auto c = c_;
        auto p = p_;
        auto f = [i, c, p](const std::vector<char>& data) {
          Json::Value result = tokens(p);
          result["thumbnail"] = util::to_base64(
              reinterpret_cast<const unsigned char*>(data.begin().base()),
              data.size());
          c(result);
        };
        if (thumbnail.left()) {
          util::enqueue([i, c, p, f]() {
            if ((i->type() == IItem::FileType::Video ||
                 i->type() == IItem::FileType::Image) &&
                !i->url().empty()) {
              try {
                std::vector<uint8_t> buffer;
                ffmpegthumbnailer::VideoThumbnailer thumbnailer;
                thumbnailer.generateThumbnail(
                    i->url(), ThumbnailerImageType::Png, buffer);
                auto ptr = reinterpret_cast<const char*>(buffer.data());
                f(std::vector<char>(ptr, ptr + buffer.size()));
              } catch (const std::exception& e) {
                util::log("couldn't generate thumbnail:", e.what());
                c(error(p, Error{IHttpRequest::Bad, e.what()}));
              }
            } else {
              c(error(
                  p, Error{IHttpRequest::Bad, "couldn't generate thumbnail"s}));
            }
          });
        } else {
          f(data_);
        }
      }
      void progress(uint32_t, uint32_t) override {}

      EitherError<IItem> item_;
      ICloudProvider::Pointer p_;
      Completed c_;
      std::vector<char> data_;
    };

    p->getThumbnailAsync(item.right(), std::make_shared<download>(item, p, c));
  });
}

Json::Value HttpCloudProvider::error(ICloudProvider::Pointer p, Error e) {
  Json::Value result;
  result["error"] = e.code_;
  result["error_description"] = e.description_;
  result["consent_url"] = p->authorizeLibraryUrl();
  return result;
}

Json::Value HttpServer::list_providers(
    const IHttpServer::IConnection& connection) const {
  Json::Value result;
  const char* key = connection.getParameter("key");
  auto p = ICloudStorage::create()->providers();
  Json::Value array(Json::arrayValue);
  for (auto t : p) {
    if (auto hints = config_.hints(t->name())) {
      ICloudProvider::InitData data;
      hints->insert({"state", key + SEPARATOR + t->name()});
      data.hints_ = *hints;
      t->initialize(std::move(data));
      Json::Value v;
      v["name"] = t->name();
      v["url"] = t->authorizeLibraryUrl();
      array.append(v);
    }
  }
  result["providers"] = array;
  return result;
}

HttpCloudProvider::Pointer HttpServer::provider(const std::string& key) {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = data_.find(key);
  if (it == std::end(data_)) {
    util::log("creating provider", key);
    auto s = std::make_shared<HttpCloudProvider>(config_, key);
    data_[key] = s;
    return s;
  } else {
    return it->second;
  }
}
