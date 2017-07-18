#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <cloudstorage/ICloudProvider.h>
#include <cloudstorage/ICloudStorage.h>
#include <json/json.h>
#include <microhttpd.h>

#include "AuthorizationServer.h"
#include "GenericRequest.h"

using namespace cloudstorage;

class HttpServer;

class HttpSession {
 public:
  using Pointer = std::shared_ptr<HttpSession>;

  enum class Status { None, ConsentRequired, Accepted, Declined, Error };

  HttpSession(HttpServer*, const std::string& session_id);

  GenericRequest::Pointer request(const char* id);

  ICloudProvider::Pointer provider(MHD_Connection* connection);

  Json::Value retry(MHD_Connection* connection);

  Json::Value list_providers() const;

  Json::Value list_directory(MHD_Connection* connection);

  Status status(const ICloudProvider&);
  void set_status(const ICloudProvider&, Status);

  std::mutex& lock() { return lock_; }
  std::deque<GenericRequest::Pointer> requests() const { return requests_; }
  int add(GenericRequest::Pointer);

 private:
  std::vector<std::pair<ICloudProvider::Pointer, Status>> providers_;
  std::deque<GenericRequest::Pointer> requests_;
  int first_request_id_ = 0;
  HttpServer* http_server_;
  std::string session_id_;
  std::mutex lock_;
};

class HttpServer {
 public:
  class Callback : public ICloudProvider::ICallback {
   public:
    Callback(HttpSession* data) : session_data_(data) {}

    Status userConsentRequired(const ICloudProvider& p);
    void accepted(const ICloudProvider&);
    void declined(const ICloudProvider&);
    void error(const ICloudProvider&, const std::string&);

   private:
    HttpSession* session_data_;
  };

  HttpServer();

  HttpSession::Pointer session(const std::string& session_id);

 private:
  friend class HttpSession;

  std::unordered_map<std::string, HttpSession::Pointer> data_;
  AuthorizationServer authorization_server_;
};

#endif  // HTTP_SERVER_H
