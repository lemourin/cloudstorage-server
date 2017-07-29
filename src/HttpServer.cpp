#include "HttpServer.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace std::string_literals;

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
          DispatchServer(f, IHttpServer::Type::MultiThreaded, daemon_port_)) {}

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
      auto p = server_->provider(key + " "s + provider);
      if (c.url() == "/exchange_code"s) {
        result = p->exchange_code(c);
      } else if (c.url() == "/list_directory"s) {
        result = p->list_directory(c);
      } else if (c.url() == "/get_item_data"s) {
        result = p->get_item_data(c);
      } else if (c.url() == "/thumbnail"s) {
        result["error"] = "unimplemented";
      }
    } else {
      if (c.url() == "/list_providers"s) result = server_->list_providers(c);
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
          IHttpServer::Type::MultiThreaded, config["port"].asInt())),
      config_(config, server_factory_) {}

ICloudProvider::ICallback::Status HttpServer::Callback::userConsentRequired(
    const ICloudProvider& p) {
  std::cerr << "waiting for user consent " << p.name() << "\n";
  return Status::None;
}

void HttpServer::Callback::accepted(const ICloudProvider& p) {
  data_->set_status(HttpCloudProvider::Status::Accepted);
  std::cerr << "accepted " << p.name() << ": " << p.token() << "\n";
}

void HttpServer::Callback::declined(const ICloudProvider& p) {
  data_->set_status(HttpCloudProvider::Status::Denied);
  std::cerr << "declined " << p.name() << "\n";
}

void HttpServer::Callback::error(const ICloudProvider& p,
                                 const std::string& d) {
  data_->set_status(HttpCloudProvider::Status::Denied);
  std::cerr << "error " << d << "\n";
}

ICloudProvider::Pointer HttpCloudProvider::provider(
    const IHttpServer::IConnection& connection) {
  const char* provider = connection.getParameter("provider");
  const char* token = connection.getParameter("token");
  if (!provider) return nullptr;
  if (!provider_ || status_ == Status::Denied) {
    std::lock_guard<std::mutex> lock(lock_);
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
    data.callback_ = std::make_unique<HttpServer::Callback>(this);
    provider_->initialize(std::move(data));
  }
  return provider_;
}

Json::Value HttpCloudProvider::exchange_code(
    const IHttpServer::IConnection& connection) {
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
  result["token"] = token;
  result["provider"] = provider->name();
  return result;
}

Json::Value HttpCloudProvider::list_directory(
    const IHttpServer::IConnection& connection) {
  Json::Value result;
  const char* item_id = connection.getParameter("item_id");
  if (!item_id) {
    result["error"] = "missing item id"s;
    return result;
  }
  auto request =
      std::make_shared<ListDirectoryRequest>(provider(connection), item_id);
  return finalize_request(request);
}

Json::Value HttpCloudProvider::get_item_data(
    const IHttpServer::IConnection& connection) {
  Json::Value result;
  const char* item_id = connection.getParameter("item_id");
  if (!item_id) {
    result["error"] = "missing item id"s;
    return result;
  }
  auto request =
      std::make_shared<GetItemDataRequest>(provider(connection), item_id);
  return finalize_request(request);
}

Json::Value HttpServer::list_providers(
    const IHttpServer::IConnection& connection) const {
  Json::Value result;
  const char* key = connection.getParameter("key");
  auto p = ICloudStorage::create()->providers();
  uint32_t idx = 0;
  Json::Value array(Json::arrayValue);
  for (auto t : p)
    if (auto hints = config_.hints(t->name())) {
      ICloudProvider::InitData data;
      hints->insert({"state", key + " "s + t->name()});
      data.hints_ = *hints;
      t->initialize(std::move(data));
      Json::Value v;
      v["name"] = t->name();
      v["url"] = t->authorizeLibraryUrl();
      array.append(v);
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
