#include "GenericRequest.h"

#include "HttpServer.h"

using namespace std::string_literals;

GenericRequest::GenericRequest(ICloudProvider::Pointer p, HttpSession* session)
    : provider_(p), session_(session) {}

Json::Value GenericRequest::auth_error() const {
  Json::Value result;
  if (session_->status(*provider_) == HttpSession::Status::ConsentRequired)
    result["error"] = "consent required";
  else if (session_->status(*provider_) == HttpSession::Status::Declined)
    result["error"] = "consent declined";
  else if (session_->status(*provider_) == HttpSession::Status::Error)
    result["error"] = "internal error";
  return result;
}

ICloudProvider::Pointer GenericRequest::provider() const { return provider_; }

void GenericRequest::wait() const { semaphore_.wait(); }

void GenericRequest::notify() const { semaphore_.notify(); }

ListDirectoryRequest::ListDirectoryRequest(ICloudProvider::Pointer p,
                                           HttpSession* session,
                                           const char* item_id)
    : GenericRequest(p, session) {
  if (!provider()) return;
  if (item_id != "root"s)
    item_request_ = provider()->getItemDataAsync(item_id, [this](auto item) {
      if (!item)
        this->notify();
      else {
        std::lock_guard<std::mutex> lock(lock_);
        request_ = this->provider()->listDirectoryAsync(
            item, [this](auto) { this->notify(); });
      }
    });
  else {
    std::lock_guard<std::mutex> lock(lock_);
    request_ = this->provider()->listDirectoryAsync(
        this->provider()->rootDirectory(), [this](auto) { this->notify(); });
  }
}

ListDirectoryRequest::~ListDirectoryRequest() {
  if (item_request_) item_request_->cancel();
  if (request_) request_->cancel();
}

Json::Value ListDirectoryRequest::result() const {
  Json::Value result = auth_error();
  if (result.isMember("error")) return result;

  bool wait = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (!request_) wait = true;
  }
  if (wait) this->wait();
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (!request_) {
      result["error"] = "invalid item";
      return result;
    }
  }
  result["token"] = provider()->token();
  result["hints"] = to_json(provider()->hints());
  Json::Value array;
  for (auto i : request_->result()) {
    Json::Value v;
    v["id"] = i->id();
    v["filename"] = i->filename();
    array.append(v);
  }
  result["items"] = array;
  return result;
}
