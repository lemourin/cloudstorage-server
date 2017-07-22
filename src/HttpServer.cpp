#include "HttpServer.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace std::string_literals;

const uint16_t AUTHORIZATION_SERVER_PORT = 12345;

namespace {

template <class Request>
Json::Value finalize_request(HttpSession* session, Request request) {
  if (!request->provider()) {
    Json::Value result;
    result["error"] = "invalid provider"s;
    return result;
  }

  return request->result();
}

}  // namespace

HttpServer::HttpServer(const std::string& hostname)
    : hostname_(hostname),
      mega_daemon_(IHttpServer::Type::MultiThreaded, 12346) {}

ICloudProvider::ICallback::Status HttpServer::Callback::userConsentRequired(
    const ICloudProvider& p) {
  std::cerr << "waiting for user consent " << p.name() << "\n";
  return Status::None;
}

void HttpServer::Callback::accepted(const ICloudProvider& p) {
  data_->status_ = HttpSession::ProviderData::Status::Accepted;
  std::cerr << "accepted " << p.name() << ": " << p.token() << "\n";
}

void HttpServer::Callback::declined(const ICloudProvider& p) {
  data_->status_ = HttpSession::ProviderData::Status::Denied;
  std::cerr << "declined " << p.name() << "\n";
}

void HttpServer::Callback::error(const ICloudProvider& p,
                                 const std::string& d) {
  data_->status_ = HttpSession::ProviderData::Status::Denied;
  std::cerr << "error " << d << "\n";
}

HttpSession::HttpSession(HttpServer* server, const std::string& session)
    : http_server_(server), session_id_(session) {}

ICloudProvider::Pointer HttpSession::provider(MHD_Connection* connection) {
  const char* provider = MHD_lookup_connection_value(
      connection, MHD_GET_ARGUMENT_KIND, "provider");
  const char* token =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "token");
  if (!provider) return nullptr;
  auto p = providers_.find(provider);
  if (p == std::end(providers_) ||
      p->second->status_ == ProviderData::Status::Denied ||
      (token && p->second->provider_->token() != token)) {
    auto r = ICloudStorage::create()->provider(provider);
    if (!r) return nullptr;
    if (p != std::end(providers_)) providers_.erase(p);
    p = providers_
            .insert({provider, std::make_shared<ProviderData>(
                                   r, ProviderData::Status::None)})
            .first;
    ICloudProvider::InitData data;
    if (token) data.token_ = token;
    data.http_server_ =
        std::make_unique<ServerFactory>(&http_server_->mega_daemon_);
    const char* access_token = MHD_lookup_connection_value(
        connection, MHD_GET_ARGUMENT_KIND, "access_token");
    if (access_token) data.hints_["access_token"] = access_token;
    data.hints_["state"] = session_id_ + "$$" + provider;
    data.hints_["youtube_dl_url"] = "http://lemourin.ddns.net:9191";
    data.hints_["redirect_uri_host"] = hostname();
    data.callback_ = std::make_unique<HttpServer::Callback>(p->second.get());
    r->initialize(std::move(data));
  }
  return p->second->provider_;
}

Json::Value HttpSession::exchange_code(MHD_Connection* connection) {
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

Json::Value HttpSession::list_providers() const {
  Json::Value result;
  auto p = ICloudStorage::create()->providers();
  uint32_t idx = 0;
  Json::Value array;
  array.resize(p.size());
  for (auto t : p) {
    ICloudProvider::InitData data;
    data.hints_["state"] = session_id_ + "$$" + t->name();
    t->initialize(std::move(data));
    Json::Value v;
    v["name"] = t->name();
    v["url"] = t->authorizeLibraryUrl();
    array[idx++] = v;
  }
  result["providers"] = array;
  return result;
}

Json::Value HttpSession::list_directory(MHD_Connection* connection) {
  Json::Value result;
  const char* item_id =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "item_id");
  if (!item_id) {
    result["error"] = "missing item id"s;
    return result;
  }
  auto request = std::make_shared<ListDirectoryRequest>(provider(connection),
                                                        this, item_id);
  return finalize_request(this, request);
}

Json::Value HttpSession::get_item_data(MHD_Connection* connection) {
  Json::Value result;
  const char* item_id =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "item_id");
  if (!item_id) {
    result["error"] = "missing item id"s;
    return result;
  }
  auto request =
      std::make_shared<GetItemDataRequest>(provider(connection), this, item_id);
  return finalize_request(this, request);
}

std::string HttpSession::hostname() const { return http_server_->hostname_; }

HttpSession::Pointer HttpServer::session(const std::string& session_id) {
  auto it = data_.find(session_id);
  if (it == std::end(data_)) {
    std::cerr << "creating session " << session_id << "\n";
    auto s = std::make_shared<HttpSession>(this, session_id);
    data_[session_id] = s;
    return s;
  } else {
    return it->second;
  }
}
