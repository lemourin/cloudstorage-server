#include "HttpServer.h"

#include <libffmpegthumbnailer/videothumbnailer.h>
#include <condition_variable>
#include <fstream>
#include <queue>
#include <sstream>

#include "CurlHttp.h"
#include "Utility.h"

using namespace std::string_literals;
using namespace std::placeholders;

const std::string SEPARATOR = "--";

namespace {

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

CloudConfig::CloudConfig(const Json::Value& config)
    : auth_url_(config["auth_url"].asString()),
      file_url_(config["file_url"].asString()),
      youtube_dl_url_(config["youtube_dl_url"].asString()),
      keys_(config["keys"]),
      secure_(!config["ssl_key"].empty()) {}

std::unique_ptr<ICloudProvider::Hints> CloudConfig::hints(
    const std::string& provider) const {
  if (!keys_.isMember(provider)) return nullptr;
  auto p = std::make_unique<ICloudProvider::Hints>();
  auto& hints = *p;
  hints["youtube_dl_url"] = youtube_dl_url_;
  hints["client_id"] = keys_[provider]["client_id"].asString();
  hints["client_secret"] = keys_[provider]["client_secret"].asString();
  hints["redirect_uri"] = auth_url_;
  hints["file_url"] = file_url_ + "/" + provider;
  return p;
}

IHttpServer::IResponse::Pointer
HttpServer::ConnectionCallback::receivedConnection(
    const IHttpServer& d, IHttpServer::IConnection::Pointer c) {
  Json::Value result(Json::objectValue);
  if (c->url() == "/quit") {
    server_->semaphore_.notify();
  } else {
    const char* provider = c->getParameter("provider");
    const char* key = c->getParameter("key");
    if (provider && key) {
      auto p = server_->provider(key + SEPARATOR + provider);
      auto r = p->provider(server_, *c);
      if (!r) {
        result["error"] = "invalid provider";
      } else {
        auto start_time = std::chrono::system_clock::now();
        auto stream = std::make_shared<std::stringstream>();
        auto cb = std::make_unique<ResponseCallback>(stream);
        auto url = c->url();
        auto func = [=](auto e) {
          *stream << Json::StyledWriter().write(e);
          util::log(url, "lasted",
                    std::chrono::duration<double>(
                        std::chrono::system_clock::now() - start_time)
                        .count());
          c->resume();
        };
        c->suspend();
        if (c->url() == "/exchange_code"s) {
          p->exchange_code(r, server_, c->getParameter("code"), func);
        } else if (c->url() == "/list_directory"s) {
          p->list_directory(r, server_, c->getParameter("item_id"), func);
        } else if (c->url() == "/get_item_data"s) {
          p->get_item_data(r, server_, c->getParameter("item_id"), func);
        } else if (c->url() == "/thumbnail"s) {
          p->thumbnail(r, server_, c->getParameter("item_id"), func);
        } else {
          result["error"] = "bad request";
          func(result);
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

  if (c->url() != "/health_check") util::log(c->url(), "received");

  auto str = Json::StyledWriter().write(result);
  return d.createResponse(200, {{"Content-Type", "application/json"}}, str);
}

HttpServer::HttpServer(Json::Value config)
    : done_(),
      clean_up_thread_([=]() {
        while (!done_) {
          std::unique_lock<std::mutex> lock(pending_requests_mutex_);
          pending_requests_condition_.wait(
              lock, [=]() { return !pending_requests_.empty() || done_; });
          while (!pending_requests_.empty()) {
            auto r = pending_requests_.back();
            pending_requests_.pop_back();
            lock.unlock();
            r->finish();
            lock.lock();
          }
        }
      }),
      server_port_(config["port"].asInt()),
      server_factory_(std::make_unique<MicroHttpdServerFactory>(
          util::read_file(config["ssl_cert"].asString()),
          util::read_file(config["ssl_key"].asString()))),
      main_server_(
          DispatchServer(server_factory_, server_port_,
                         std::bind(&HttpServer::proxy, this, _1, _2, _3))),
      query_server_(main_server_, "",
                    std::make_unique<ConnectionCallback>(this)),
      config_(config) {}

HttpServer::~HttpServer() {
  done_ = true;
  pending_requests_condition_.notify_one();
  clean_up_thread_.join();
  data_.clear();
}

IHttpServer::IResponse::Pointer HttpServer::proxy(
    const IHttpServer& server, IHttpServer::IConnection::Pointer connection,
    const DispatchServer::Callback& callback) {
  if (connection->url() == "/files") {
    auto key = connection->getParameter("key");
    auto provider = connection->getParameter("provider");
    auto file = connection->getParameter("file");
    auto state = connection->getParameter("state");
    if (!key || !provider || !file || !state)
      return server.createResponse(IHttpRequest::Bad, {}, "");
    auto c = this->provider(key + SEPARATOR + provider);
    auto provider_object = c->provider(this, *connection);
    if (!provider_object) return nullptr;
    c->get_item_data(provider_object, this, connection->getParameter("file"),
                     [](auto) {});
    auto f = callback.callback(state);
    if (!f) return nullptr;
    return f->receivedConnection(server, connection);
  }
  return nullptr;
}

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
    HttpServer* server, const IHttpServer::IConnection& connection) {
  const char* provider = connection.getParameter("provider");
  const char* token = connection.getParameter("token");
  if (!provider || !token) return nullptr;
  std::lock_guard<std::mutex> lock(lock_);
  if (!provider_ || provider_->token() != token || status_ == Status::Denied) {
    ICloudProvider::InitData data;
    if (token) data.token_ = token;
    data.http_server_ =
        std::make_unique<ServerWrapperFactory>(server->main_server_);
    data.http_engine_ = std::make_unique<CurlHttp>();
    const char* access_token = connection.getParameter("access_token");
    data.hints_ = *config_.hints(provider);
    if (access_token) data.hints_["access_token"] = access_token;
    data.hints_["state"] = key_;
    data.callback_ = std::make_unique<HttpServer::AuthCallback>(this);
    provider_ = ICloudStorage::create()->provider(provider, std::move(data));
  }
  return provider_;
}

void HttpCloudProvider::exchange_code(ICloudProvider::Pointer p,
                                      HttpServer* server, const char* code,
                                      Completed c) {
  if (!code) {
    Json::Value result;
    result["error"] = "missing code";
    return c(result);
  }
  server->add(p->exchangeCodeAsync(code, [=](auto token) {
    if (token.right()) {
      Json::Value result;
      result["token"] = token.right()->token_;
      result["access_token"] = token.right()->access_token_;
      result["provider"] = p->name();
      c(result);
    } else {
      c(error(p, *token.left()));
    }
  }));
}

void HttpCloudProvider::list_directory(ICloudProvider::Pointer p,
                                       HttpServer* server, const char* item_id,
                                       Completed c) {
  item(p, server, item_id, [=](auto item) {
    if (item.left()) return c(error(p, *item.left()));
    server->add(p->listDirectoryAsync(item.right(), [=](auto list) {
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
    }));
  });
}

void HttpCloudProvider::get_item_data(ICloudProvider::Pointer p,
                                      HttpServer* server, const char* item_id,
                                      Completed c) {
  item(p, server, item_id, [=](auto item) {
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

void HttpCloudProvider::item(ICloudProvider::Pointer p, HttpServer* server,
                             const char* item_id, CompletedItem c) {
  if (!item_id)
    c(Error{IHttpRequest::NotFound, "not found"});
  else
    item_id == "root"s ? c(p->rootDirectory())
                       : server->add(p->getItemDataAsync(item_id, c));
}

void HttpCloudProvider::thumbnail(ICloudProvider::Pointer p, HttpServer* server,
                                  const char* item_id, Completed c) {
  item(p, server, item_id, [=](auto item) {
    if (item.left()) return c(error(p, *item.left()));

    class download : public IDownloadFileCallback {
     public:
      download(EitherError<IItem> item, ICloudProvider::Pointer p, bool secure,
               uint16_t port, Completed c)
          : item_(item), p_(p), secure_(secure), port_(port), c_(c) {}

      void receivedData(const char* data, uint32_t length) override {
        for (size_t i = 0; i < length; i++) data_.push_back(data[i]);
      }
      void done(EitherError<void> thumbnail) override {
        auto i = item_.right();
        auto c = c_;
        auto p = p_;
        auto secure = secure_;
        auto port = port_;
        auto f = [=](const std::vector<char>& data) {
          Json::Value result = tokens(p);
          result["thumbnail"] = util::to_base64(
              reinterpret_cast<const unsigned char*>(data.begin().base()),
              data.size());
          c(result);
        };
        if (thumbnail.left()) {
          util::enqueue([=]() {
            if ((i->type() == IItem::FileType::Video ||
                 i->type() == IItem::FileType::Image) &&
                !i->url().empty()) {
              try {
                std::vector<uint8_t> buffer;
                ffmpegthumbnailer::VideoThumbnailer thumbnailer;
                auto file_url = p->hints()["file_url"];
                auto url = i->url();
                if (!file_url.empty()) {
                  if (url.substr(0, file_url.length()) == file_url) {
                    auto rest =
                        std::string(url.begin() + file_url.length(), url.end());
                    url = (secure ? "https" : "http") + "://localhost:"s +
                          std::to_string(port) + rest;
                  }
                }
                thumbnailer.generateThumbnail(url, ThumbnailerImageType::Png,
                                              buffer);
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
      bool secure_;
      uint16_t port_;
      Completed c_;
      std::vector<char> data_;
    };

    server->add(p->getThumbnailAsync(
        item.right(),
        std::make_shared<download>(item, p, server->config_.secure_,
                                   server->server_port_, c)));
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
  if (!key) key = "";
  Json::Value array(Json::arrayValue);
  for (auto t : ICloudStorage::create()->providers()) {
    if (auto hints = config_.hints(t)) {
      ICloudProvider::InitData data;
      hints->insert({"state", key + SEPARATOR + t});
      data.hints_ = *hints;
      data.http_server_ = std::make_unique<MicroHttpdServerFactory>("", "");
      data.http_engine_ = std::make_unique<CurlHttp>();
      auto p = ICloudStorage::create()->provider(t, std::move(data));
      Json::Value v;
      v["name"] = p->name();
      v["url"] = p->authorizeLibraryUrl();
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

void HttpServer::add(std::shared_ptr<IGenericRequest> r) {
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    pending_requests_.push_back(r);
  }
  pending_requests_condition_.notify_one();
}

int HttpServer::exec() {
  semaphore_.wait();
  return 0;
}
