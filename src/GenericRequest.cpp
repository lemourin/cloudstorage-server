#include "GenericRequest.h"

#include "HttpServer.h"

#include <iostream>

using namespace std::string_literals;

namespace {

class ListDirectoryCb : public IListDirectoryCallback {
 public:
  ListDirectoryCb(ListDirectoryRequest* r) : request_(r), error_(false) {}

  void receivedItem(IItem::Pointer item) {}

  void done(const std::vector<IItem::Pointer>& result) { request_->notify(); }

  void error(const std::string& description) {
    error_ = true;
    std::cerr << description << "\n";
    request_->notify();
  }

  bool error_occured() const { return error_; }

 private:
  ListDirectoryRequest* request_;
  bool error_;
};

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

}  // namespace

GenericRequest::GenericRequest(ICloudProvider::Pointer p, HttpSession* session)
    : provider_(p), session_(session) {}

ICloudProvider::Pointer GenericRequest::provider() const { return provider_; }

void GenericRequest::wait() const { semaphore_.wait(); }

void GenericRequest::notify() const { semaphore_.notify(); }

ListDirectoryRequest::ListDirectoryRequest(ICloudProvider::Pointer p,
                                           HttpSession* session,
                                           const char* item_id)
    : GenericRequest(p, session),
      callback_(std::make_shared<ListDirectoryCb>(this)) {
  if (!provider()) return;
  if (item_id != "root"s)
    item_request_ = provider()->getItemDataAsync(item_id, [this](auto item) {
      if (!item) {
        this->notify();
      } else {
        std::lock_guard<std::mutex> lock(lock_);
        item_ = item;
        request_ = this->provider()->listDirectoryAsync(item, callback_);
      }
    });
  else {
    std::lock_guard<std::mutex> lock(lock_);
    item_ = this->provider()->rootDirectory();
    request_ = this->provider()->listDirectoryAsync(
        this->provider()->rootDirectory(), callback_);
  }
}

ListDirectoryRequest::~ListDirectoryRequest() {
  if (item_request_) item_request_->cancel();
  if (request_) request_->cancel();
}

bool ListDirectoryRequest::should_wait() const {
  std::lock_guard<std::mutex> lock(lock_);
  return !request_;
}

Json::Value ListDirectoryRequest::result() const {
  Json::Value result;

  if (should_wait()) this->wait();
  if (should_wait()) {
    result["error"] = "invalid item";
    result["consent_url"] = provider()->authorizeLibraryUrl();
    return result;
  }
  Json::Value array(Json::arrayValue);
  for (auto i : request_->result()) {
    Json::Value v;
    v["id"] = i->id();
    v["filename"] = i->filename();
    v["type"] = file_type_to_string(i->type());
    array.append(v);
  }
  if (static_cast<ListDirectoryCb*>(callback_.get())->error_occured()) {
    result["error"] = "error";
    result["consent_url"] = provider()->authorizeLibraryUrl();
  } else {
    result["items"] = array;
    result["token"] = provider()->token();
    result["access_token"] = provider()->hints()["access_token"];
    result["provider"] = provider()->name();
  }
  return result;
}

GetItemDataRequest::GetItemDataRequest(ICloudProvider::Pointer p,
                                       HttpSession* session,
                                       const char* item_id)
    : GenericRequest(p, session) {
  if (!provider()) return;
  request_ = p->getItemDataAsync(item_id, [this](auto) { this->notify(); });
}

GetItemDataRequest::~GetItemDataRequest() { request_ = nullptr; }

Json::Value GetItemDataRequest::result() const {
  Json::Value r;
  auto item = request_->result();
  if (!item) {
    r["error"] = "error occured";
    return r;
  }
  r["url"] = item->url();
  return r;
}
