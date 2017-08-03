#include "HttpServer.h"

#include <libffmpegthumbnailer/videothumbnailer.h>
#include <condition_variable>
#include <fstream>
#include <queue>
#include <sstream>

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
  }
}

Json::Value tokens(ICloudProvider::Pointer p) {
  Json::Value result;
  result["token"] = p->token();
  auto hints = p->hints();
  auto it = hints.find("access_token");
  if (it != std::end(hints)) result["access_token"] = it->second;
  return result;
}

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
      } else if (c->url() == "/exchange_code"s) {
        result = p->exchange_code(r, *c);
      } else if (c->url() == "/list_directory"s) {
        result = p->list_directory(r, *c);
      } else if (c->url() == "/get_item_data"s) {
        result = p->get_item_data(r, *c);
      } else if (c->url() == "/thumbnail"s) {
        result = p->thumbnail(r, *c);
      }
    } else {
      if (c->url() == "/list_providers"s)
        result = server_->list_providers(*c);
      else
        result["error"] = "invalid request";
    }
  }

  auto str = Json::StyledWriter().write(result);
  util::log("sending", str.length(), "bytes");
  return d.createResponse(200, {{"Content-Type", "application/json"}}, str);
}

HttpServer::HttpServer(Json::Value config)
    : server_factory_(std::make_unique<MicroHttpdServerFactory>(
          util::read_file(config["ssl_cert"].asString()),
          util::read_file(config["ssl_key"].asString()))),
      main_server_(server_factory_->create(
          std::make_unique<ConnectionCallback>(this), "",
          MicroHttpdServer::Type::MultiThreaded, config["port"].asInt())),
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
    const char* access_token = connection.getParameter("access_token");
    data.hints_ = *config_.hints(provider);
    if (access_token) data.hints_["access_token"] = access_token;
    data.hints_["state"] = key_;
    data.callback_ = std::make_unique<HttpServer::AuthCallback>(this);
    provider_->initialize(std::move(data));
  }
  return provider_;
}

Json::Value HttpCloudProvider::exchange_code(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  const char* code = connection.getParameter("code");
  if (!code) {
    Json::Value result;
    result["error"] = "missing code";
    return result;
  }
  auto token = p->exchangeCodeAsync(code)->result();
  if (token.right()) {
    Json::Value result;
    result["token"] = *token.right();
    result["provider"] = p->name();
    return result;
  } else {
    return error(p, *token.left());
  }
}

Json::Value HttpCloudProvider::list_directory(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  auto item = this->item(p, connection);
  if (item.left()) return error(p, *item.left());
  auto list = p->listDirectoryAsync(item.right())->result();
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
    return result;
  } else {
    return error(p, *list.left());
  }
}

Json::Value HttpCloudProvider::get_item_data(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  auto item = this->item(p, connection);
  if (item.left()) {
    return error(p, *item.left());
  } else {
    Json::Value result = tokens(p);
    result["url"] = item.right()->url();
    result["id"] = item.right()->id();
    return result;
  }
}

EitherError<IItem> HttpCloudProvider::item(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  const char* item_id = connection.getParameter("item_id");
  if (!item_id) return Error{IHttpRequest::NotFound, "not found"};
  return item_id == "root"s ? p->rootDirectory()
                            : p->getItemDataAsync(item_id)->result();
}

Json::Value HttpCloudProvider::thumbnail(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  auto item = this->item(p, connection);
  if (item.left()) return error(p, *item.left());

  class download : public IDownloadFileCallback {
   public:
    void receivedData(const char* data, uint32_t length) override {
      for (size_t i = 0; i < length; i++) data_.push_back(data[i]);
    }
    void done(EitherError<void>) override {}
    void progress(uint32_t, uint32_t) override {}

    std::vector<char> data_;
  };

  auto callback = std::make_shared<download>();
  auto thumbnail = p->getThumbnailAsync(item.right(), callback)->result();
  if (thumbnail.left()) {
    auto i = item.right();
    callback->data_ = {};
    if ((i->type() == IItem::FileType::Video ||
         i->type() == IItem::FileType::Image) &&
        !i->url().empty()) {
      try {
        std::vector<uint8_t> buffer;
        ffmpegthumbnailer::VideoThumbnailer thumbnailer;
        thumbnailer.generateThumbnail(i->url(), ThumbnailerImageType::Png,
                                      buffer);
        auto ptr = reinterpret_cast<const char*>(buffer.data());
        callback->data_ = std::vector<char>(ptr, ptr + buffer.size());
      } catch (const std::exception& e) {
        util::log("couldn't generate thumbnail:", e.what());
      }
    }
    if (callback->data_.empty()) return error(p, *thumbnail.left());
  }
  Json::Value result = tokens(p);
  result["thumbnail"] = util::to_base64(
      reinterpret_cast<unsigned char*>(callback->data_.begin().base()),
      callback->data_.size());
  return result;
}

Json::Value HttpCloudProvider::error(ICloudProvider::Pointer p, Error e) const {
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
  uint32_t idx = 0;
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
