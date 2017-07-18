#ifndef GENERIC_REQUEST_H
#define GENERIC_REQUEST_H

#include <cloudstorage/ICloudProvider.h>
#include <json/json.h>
#include <microhttpd.h>

#include <memory>
#include <string>

#include "Utility.h"

using namespace cloudstorage;

class HttpSession;

class GenericRequest {
 public:
  using Pointer = std::shared_ptr<GenericRequest>;

  GenericRequest(ICloudProvider::Pointer, HttpSession*);

  virtual ~GenericRequest() = default;

  virtual Json::Value result() const = 0;

  Json::Value auth_error() const;

  ICloudProvider::Pointer provider() const;

  void wait() const;
  void notify() const;

 private:
  ICloudProvider::Pointer provider_;
  mutable Semaphore semaphore_;
  HttpSession* session_;
};

class ListDirectoryRequest : public GenericRequest {
 public:
  ListDirectoryRequest(ICloudProvider::Pointer, HttpSession*,
                       const char* item_id);
  ~ListDirectoryRequest();

  Json::Value result() const;

 private:
  mutable std::mutex lock_;
  ICloudProvider::GetItemRequest::Pointer item_request_;
  ICloudProvider::ListDirectoryRequest::Pointer request_;
};

#endif  // GENERIC_REQUEST_H
