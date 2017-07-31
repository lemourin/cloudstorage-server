#include "HttpServer.h"

#include <condition_variable>
#include <fstream>
#include <iostream>
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

void insert_tokens(ICloudProvider::Pointer p, Json::Value& result) {
  result["token"] = p->token();
  auto hints = p->hints();
  auto it = hints.find("access_token");
  if (it != std::end(hints)) result["access_token"] = it->second;
}

}  // namespace

CloudConfig::CloudConfig(const Json::Value& config,
                         IHttpServerFactory::Pointer f)
    : auth_url_(config["auth_url"].asString()),
      auth_port_(config["auth_port"].asInt()),
      file_url_(config["file_url"].asString()),
      daemon_port_(config["daemon_port"].asInt()),
      public_daemon_port_(config["public_daemon_port"].asInt()),
      youtube_dl_url_(config["youtube_dl_url"].asString()),
      keys_(config["keys"]),
      file_daemon_(
          DispatchServer(f, IHttpServer::Type::FileProvider, daemon_port_)) {}

std::unique_ptr<ICloudProvider::Hints> CloudConfig::hints(
    const std::string& provider) const {
  if (!keys_.isMember(provider)) return nullptr;
  auto p = std::make_unique<ICloudProvider::Hints>();
  auto& hints = *p;
  hints["youtube_dl_url"] = youtube_dl_url_;
  hints["client_id"] = keys_[provider]["client_id"].asString();
  hints["client_secret"] = keys_[provider]["client_secret"].asString();
  hints["redirect_uri_host"] = auth_url_;
  hints["redirect_uri_port"] = std::to_string(auth_port_);
  hints["daemon_port"] = std::to_string(public_daemon_port_);
  hints["file_url"] = file_url_;
  return p;
}

IHttpServer::IResponse::Pointer
HttpServer::ConnectionCallback::receivedConnection(
    const IHttpServer& d, const IHttpServer::IConnection& c) {
  std::cerr << "got request " << c.url() << "\n";
  Json::Value result;
  const char* key = c.getParameter("key");
  if (!key) {
    result["error"] = "missing key";
  } else {
    const char* provider = c.getParameter("provider");
    if (provider) {
      auto p = server_->provider(key + SEPARATOR + provider);
      auto r = p->provider(c);
      if (!r) {
        result["error"] = "invalid provider";
      } else if (c.url() == "/exchange_code"s) {
        result = p->exchange_code(r, c);
      } else if (c.url() == "/list_directory"s) {
        result = p->list_directory(r, c);
      } else if (c.url() == "/get_item_data"s) {
        result = p->get_item_data(r, c);
      } else if (c.url() == "/thumbnail"s) {
        return p->thumbnail(r, d, c);
      }
    } else {
      if (c.url() == "/list_providers"s)
        result = server_->list_providers(c);
      else
        result["error"] = "invalid request";
    }
  }

  auto str = Json::StyledWriter().write(result);
  return d.createResponse(200, {{"Content-Type", "application/json"}}, str);
}

HttpServer::HttpServer(Json::Value config)
    : server_factory_(std::make_unique<MicroHttpdServerFactory>(
          read_file(config["ssl_cert"].asString()),
          read_file(config["ssl_key"].asString()))),
      main_server_(server_factory_->create(
          std::make_unique<ConnectionCallback>(this), "",
          MicroHttpdServer::Type::MultiThreaded, config["port"].asInt())),
      config_(config, server_factory_) {}

ICloudProvider::IAuthCallback::Status
HttpServer::AuthCallback::userConsentRequired(const ICloudProvider& p) {
  std::cerr << "waiting for user consent " << p.name() << "\n";
  return Status::None;
}

void HttpServer::AuthCallback::done(const ICloudProvider& p,
                                    EitherError<void> e) {
  if (e.left()) {
    data_->set_status(HttpCloudProvider::Status::Denied);
    std::cerr << "auth error " << e.left()->code_ << ": "
              << e.left()->description_ << "\n";
  } else {
    data_->set_status(HttpCloudProvider::Status::Accepted);
    std::cerr << "accepted " << p.name() << ": " << p.token() << "\n";
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
  auto provider = this->provider(connection);
  Json::Value result;
  const char* code = connection.getParameter("code");
  if (!code || !provider) {
    std::string msg = "missing";
    if (!code) msg += " code";
    if (!provider) msg += " provider";
    result["error"] = msg;
    return result;
  }
  auto token = provider->exchangeCodeAsync(code)->result();
  if (token.right()) {
    result["token"] = *token.right();
    result["provider"] = provider->name();
  } else {
    result["error"] = token.left()->description_;
  }
  return result;
}

Json::Value HttpCloudProvider::list_directory(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  Json::Value result;
  auto item = this->item(p, connection);
  if (item.left()) {
    result["error"] = item.left()->description_;
    result["consent_url"] = p->authorizeLibraryUrl();
    std::cerr << "error " << item.left()->code_ << ": "
              << item.left()->description_ << "\n";
    return result;
  }
  Json::Value array(Json::arrayValue);
  auto list = p->listDirectoryAsync(item.right())->result();
  if (list.right()) {
    for (auto i : *list.right()) {
      Json::Value v;
      v["id"] = i->id();
      v["filename"] = i->filename();
      v["type"] = file_type_to_string(i->type());
      array.append(v);
    }
    result["items"] = array;
    insert_tokens(p, result);
  } else {
    result["error"] = list.left()->description_;
    result["consent_url"] = p->authorizeLibraryUrl();
    std::cerr << "error " << list.left()->code_ << ": "
              << list.left()->description_ << "\n";
  }
  return result;
}

Json::Value HttpCloudProvider::get_item_data(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  Json::Value result;
  const char* item_id = connection.getParameter("item_id");
  if (!item_id) {
    result["error"] = "missing item id"s;
    return result;
  }
  auto item = p->getItemDataAsync(item_id)->result();
  if (item.left()) {
    result["error"] = item.left()->description_;
    result["consent_url"] = p->authorizeLibraryUrl();
    std::cerr << "error while getting " << item_id << ", provider=" << p->name()
              << " " << item.left()->code_ << ": " << item.left()->description_
              << "\n";
    return result;
  } else {
    result["url"] = item.right()->url();
    result["id"] = item.right()->id();
    insert_tokens(p, result);
  }
  return result;
}

EitherError<IItem> HttpCloudProvider::item(
    ICloudProvider::Pointer p, const IHttpServer::IConnection& connection) {
  const char* item_id = connection.getParameter("item_id");
  if (!item_id) return Error{IHttpRequest::NotFound, "not found"};
  return item_id == "root"s ? p->rootDirectory()
                            : p->getItemDataAsync(item_id)->result();
}

IHttpServer::IResponse::Pointer HttpCloudProvider::thumbnail(
    ICloudProvider::Pointer p, const IHttpServer& server,
    const IHttpServer::IConnection& connection) {
  auto item = this->item(p, connection);
  if (item.left())
    return server.createResponse(item.left()->code_, {},
                                 item.left()->description_);
  struct buffer {
    using Pointer = std::shared_ptr<buffer>;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<char> data_;
    bool done_ = false;
  };

  class upload : public IHttpServer::IResponse::ICallback {
   public:
    upload(ICloudProvider::DownloadFileRequest::Pointer r, buffer::Pointer b)
        : request_(r), buffer_(b) {}
    int putData(char* buffer, size_t size) override {
      std::unique_lock<std::mutex> lock(buffer_->mutex_);
      buffer_->condition_.wait(
          lock, [this]() { return !buffer_->data_.empty() || buffer_->done_; });
      int n = std::min(buffer_->data_.size(), size);
      for (int i = 0; i < n; i++) {
        buffer[i] = buffer_->data_.front();
        buffer_->data_.pop();
      }
      return n == 0 ? -1 : n;
    }
    ICloudProvider::DownloadFileRequest::Pointer request_;
    buffer::Pointer buffer_;
  };

  class download : public IDownloadFileCallback {
   public:
    download(buffer::Pointer b) : buffer_(b) {}
    void receivedData(const char* data, uint32_t length) {
      {
        std::lock_guard<std::mutex> lock(buffer_->mutex_);
        for (size_t i = 0; i < length; i++) buffer_->data_.push(data[i]);
      }
      buffer_->condition_.notify_one();
    }
    void done(EitherError<void> e) {
      {
        std::lock_guard<std::mutex> lock(buffer_->mutex_);
        buffer_->done_ = true;
      }
      if (e.left())
        std::cerr << "thumbnail error " << e.left()->code_ << ": "
                  << e.left()->description_ << "\n";
      buffer_->condition_.notify_one();
    }
    void progress(uint32_t, uint32_t) {}
    buffer::Pointer buffer_;
  };

  auto buf = std::make_shared<buffer>();
  return server.createResponse(
      IHttpRequest::Ok, {{"Content-Type", "image/png"}}, -1, 1024,
      std::make_unique<upload>(
          p->getThumbnailAsync(item.right(), std::make_shared<download>(buf)),
          buf));
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
    std::cerr << "creating provider " << key << "\n";
    auto s = std::make_shared<HttpCloudProvider>(config_, key);
    data_[key] = s;
    return s;
  } else {
    return it->second;
  }
}
