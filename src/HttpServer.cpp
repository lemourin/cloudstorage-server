#include "HttpServer.h"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace std::string_literals;

const uint16_t AUTHORIZATION_SERVER_PORT = 12345;

namespace {

template <class Request>
Json::Value finalize_request(HttpSession* session, Request request) {
  int request_id = session->add(request);
  if (session->status(*request->provider()) !=
      HttpSession::Status::ConsentRequired) {
    request->wait();
  }
  if (session->status(*request->provider()) ==
      HttpSession::Status::ConsentRequired) {
    Json::Value result;
    result["error"] = "consent required";
    result["consent_url"] = request->provider()->authorizeLibraryUrl();
    result["id"] = request_id;
    return result;
  } else {
    return request->result();
  }
}

}  // namespace

HttpServer::HttpServer()
    : authorization_server_(IHttpServer::Type::SingleThreaded,
                            AUTHORIZATION_SERVER_PORT) {}

ICloudProvider::ICallback::Status HttpServer::Callback::userConsentRequired(
    const ICloudProvider& p) {
  std::cerr << "waiting for user consent " << p.name() << "\n";
  session_data_->set_status(p, HttpSession::Status::ConsentRequired);
  {
    std::lock_guard<std::mutex> lock(session_data_->lock());
    for (auto r : session_data_->requests()) r->notify();
  }
  return Status::WaitForAuthorizationCode;
}

void HttpServer::Callback::accepted(const ICloudProvider& p) {
  std::cerr << "accepted " << p.name() << "\n";
  session_data_->set_status(p, HttpSession::Status::Accepted);
}

void HttpServer::Callback::declined(const ICloudProvider& p) {
  std::cerr << "declined " << p.name() << "\n";
  session_data_->set_status(p, HttpSession::Status::Declined);
}

void HttpServer::Callback::error(const ICloudProvider& p,
                                 const std::string& d) {
  std::cerr << "error " << d << "\n";
  session_data_->set_status(p, HttpSession::Status::Error);
}

HttpSession::HttpSession(HttpServer* server, const std::string& session)
    : http_server_(server), session_id_(session) {}

ICloudProvider::Pointer HttpSession::provider(MHD_Connection* connection) {
  const char* provider = MHD_lookup_connection_value(
      connection, MHD_GET_ARGUMENT_KIND, "provider");
  if (!provider) provider = "";
  ICloudProvider::Pointer p = nullptr;
  for (auto r : providers_)
    if (r.first->name() == provider) p = r.first;
  if (!p) {
    p = ICloudStorage::create()->provider(provider);
    if (!p) return nullptr;
    ICloudProvider::InitData data;
    data.hints_["state"] = session_id_ + provider;
    if (p->name() == "mega" || p->name() == "owncloud" ||
        p->name() == "amazons3") {
      {
        std::fstream file(RESOURCE_PATH + "/"s + p->name() + "_login.html"s);
        std::stringstream buffer;
        buffer << file.rdbuf();
        data.hints_["login_page"] = buffer.str();
      }
      {
        std::fstream file(RESOURCE_PATH + "/"s + p->name() + "_success.html"s);
        std::stringstream buffer;
        buffer << file.rdbuf();
        data.hints_["success_page"] = buffer.str();
      }
    } else {
      std::fstream file(RESOURCE_PATH + "/default_success.html"s);
      std::stringstream buffer;
      buffer << file.rdbuf();
      data.hints_["success_page"] = buffer.str();
    }
    {
      std::fstream file(RESOURCE_PATH + "/default_error.html"s);
      std::stringstream buffer;
      buffer << file.rdbuf();
      data.hints_["error_page"] = buffer.str();
    }
    data.callback_ = std::make_unique<HttpServer::Callback>(this);
    data.http_server_ =
        std::make_unique<ServerFactory>(&http_server_->authorization_server_);
    p->initialize(std::move(data));
    providers_.push_back({p, Status::None});
  }
  return p;
}

Json::Value HttpSession::retry(MHD_Connection* connection) {
  const char* request_id = MHD_lookup_connection_value(
      connection, MHD_GET_ARGUMENT_KIND, "request_id");
  auto r = request(request_id);
  Json::Value result;
  if (!r) {
    result["error"] = "Invalid request id"s;
    return result;
  }
  std::cerr << "retrying request: " << request_id << "\n";
  auto ret = r->result();
  return ret;
}

Json::Value HttpSession::list_providers() const {
  Json::Value result;
  auto p = ICloudStorage::create()->providers();
  uint32_t idx = 0;
  Json::Value array;
  array.resize(p.size());
  for (auto t : p) array[idx++] = t->name();
  result["providers"] = array;
  return result;
}

Json::Value HttpSession::list_directory(MHD_Connection* connection) {
  Json::Value result;
  const char* item_id =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "item_id");
  if (!item_id) {
    result["error"] = "Missing item id"s;
    return result;
  }
  auto request = std::make_shared<ListDirectoryRequest>(provider(connection),
                                                        this, item_id);
  if (!request->provider()) {
    result["error"] = "Invalid provider"s;
    return result;
  }
  return finalize_request(this, request);
}

GenericRequest::Pointer HttpSession::request(const char* id) {
  std::lock_guard<std::mutex> lock(lock_);
  int i = (id != nullptr ? std::atoi(id) : 0) - first_request_id_;
  if (i >= 0 && i < requests_.size())
    return requests_[i];
  else
    return nullptr;
}

HttpSession::Status HttpSession::status(const ICloudProvider& p) {
  std::lock_guard<std::mutex> lock(lock_);
  for (auto r : providers_)
    if (r.first->name() == p.name()) return r.second;
  throw std::logic_error("invalid provider");
}

void HttpSession::set_status(const ICloudProvider& p,
                             HttpSession::Status status) {
  std::lock_guard<std::mutex> lock(lock_);
  for (auto& r : providers_)
    if (r.first->name() == p.name()) {
      r.second = status;
      return;
    }
  throw std::logic_error("invalid provider");
}

int HttpSession::add(GenericRequest::Pointer r) {
  std::lock_guard<std::mutex> lock(lock_);
  requests_.push_back(r);
  return requests_.size() - 1;
}

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
