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

HttpServer::HttpServer(Json::Value config)
    : auth_url_(config["auth_url"].asString()),
      auth_port_(config["auth_port"].asInt()),
      daemon_port_(config["daemon_port"].asInt()),
      public_daemon_port_(config["public_daemon_port"].asInt()),
      file_url_(config["file_url"].asString()),
      keys_(config["keys"]),
      file_daemon_(IHttpServer::Type::MultiThreaded, daemon_port_,
                   read_file(config["ssl_cert"].asString()),
                   read_file(config["ssl_key"].asString())) {}

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
    MHD_Connection* connection) {
  const char* provider = MHD_lookup_connection_value(
      connection, MHD_GET_ARGUMENT_KIND, "provider");
  const char* token =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "token");
  if (!provider) return nullptr;
  if (!provider_ || status_ == Status::Denied) {
    provider_ = ICloudStorage::create()->provider(provider);
    if (!provider_) return nullptr;
    ICloudProvider::InitData data;
    if (token) data.token_ = token;
    data.http_server_ =
        std::make_unique<ServerFactory>(&http_server_->file_daemon_);
    const char* access_token = MHD_lookup_connection_value(
        connection, MHD_GET_ARGUMENT_KIND, "access_token");
    if (access_token) data.hints_["access_token"] = access_token;
    data.hints_["state"] = key_;
    http_server_->initialize(provider, data.hints_);
    data.callback_ = std::make_unique<HttpServer::Callback>(this);
    provider_->initialize(std::move(data));
  }
  return provider_;
}

Json::Value HttpCloudProvider::exchange_code(MHD_Connection* connection) {
  auto provider = this->provider(connection);
  Json::Value result;
  const char* code =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "code");
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

Json::Value HttpCloudProvider::list_directory(MHD_Connection* connection) {
  Json::Value result;
  const char* item_id =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "item_id");
  if (!item_id) {
    result["error"] = "missing item id"s;
    return result;
  }
  auto request =
      std::make_shared<ListDirectoryRequest>(provider(connection), item_id);
  return finalize_request(request);
}

Json::Value HttpCloudProvider::get_item_data(MHD_Connection* connection) {
  Json::Value result;
  const char* item_id =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "item_id");
  if (!item_id) {
    result["error"] = "missing item id"s;
    return result;
  }
  auto request =
      std::make_shared<GetItemDataRequest>(provider(connection), item_id);
  return finalize_request(request);
}

bool HttpServer::initialize(const std::string& provider,
                            ICloudProvider::Hints& hints) const {
  if (!keys_.isMember(provider)) return false;
  hints["youtube_dl_url"] = "http://lemourin.ddns.net/youtube-dl";
  hints["client_id"] = keys_[provider]["client_id"].asString();
  hints["client_secret"] = keys_[provider]["client_secret"].asString();
  hints["redirect_uri_host"] = auth_url_;
  hints["redirect_uri_port"] = std::to_string(auth_port_);
  hints["daemon_port"] = std::to_string(public_daemon_port_);
  hints["file_url"] = file_url_;
  return true;
}

Json::Value HttpServer::list_providers(MHD_Connection* connection) const {
  Json::Value result;
  const char* key =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "key");
  auto p = ICloudStorage::create()->providers();
  uint32_t idx = 0;
  Json::Value array(Json::arrayValue);
  for (auto t : p) {
    ICloudProvider::InitData data;
    if (!initialize(t->name(), data.hints_)) continue;
    data.hints_["state"] = key + " "s + t->name();
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
  auto it = data_.find(key);
  if (it == std::end(data_)) {
    std::cerr << "creating provider " << key << "\n";
    auto s = std::make_shared<HttpCloudProvider>(this, key);
    data_[key] = s;
    return s;
  } else {
    return it->second;
  }
}
