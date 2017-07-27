#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <cloudstorage/ICloudProvider.h>
#include <cloudstorage/ICloudStorage.h>
#include <json/json.h>
#include <microhttpd.h>
#include <atomic>

#include "CloudServer.h"
#include "GenericRequest.h"

using namespace cloudstorage;

class HttpServer;

class HttpSession {
 public:
  using Pointer = std::shared_ptr<HttpSession>;

  struct ProviderData {
    using Pointer = std::shared_ptr<ProviderData>;

    enum class Status { None, Accepted, Denied };

    ProviderData(ICloudProvider::Pointer p, Status s)
        : provider_(p), status_(s) {}

    ICloudProvider::Pointer provider_;
    std::atomic<Status> status_;
  };

  HttpSession(HttpServer*, const std::string& session_id);

  ICloudProvider::Pointer provider(MHD_Connection* connection);

  Json::Value list_providers() const;

  Json::Value exchange_code(MHD_Connection* connection);

  Json::Value list_directory(MHD_Connection* connection);

  Json::Value get_item_data(MHD_Connection* connection);

  std::mutex& lock() const { return lock_; }

  std::string hostname() const;

  bool initialize(const std::string& provider, ICloudProvider::Hints&) const;

 private:
  HttpServer* http_server_;
  std::unordered_map<std::string, ProviderData::Pointer> providers_;
  std::string session_id_;
  mutable std::mutex lock_;
};

class HttpServer {
 public:
  class Callback : public ICloudProvider::ICallback {
   public:
    Callback(HttpSession::ProviderData* data) : data_(data) {}

    Status userConsentRequired(const ICloudProvider& p);
    void accepted(const ICloudProvider&);
    void declined(const ICloudProvider&);
    void error(const ICloudProvider&, const std::string&);

   private:
    HttpSession::ProviderData* data_;
  };

  HttpServer(Json::Value config);

  HttpSession::Pointer session(const std::string& session_id);

 private:
  friend class HttpSession;

  std::string hostname_;
  uint16_t redirect_uri_port_;
  uint16_t daemon_port_;
  uint16_t public_daemon_port_;
  std::string file_url_;
  Json::Value keys_;
  std::unordered_map<std::string, HttpSession::Pointer> data_;
  CloudServer mega_daemon_;
};

#endif  // HTTP_SERVER_H
