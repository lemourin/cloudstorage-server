#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <cloudstorage/ICloudProvider.h>
#include <cloudstorage/ICloudStorage.h>
#include <json/json.h>
#include <atomic>

#include "DispatchServer.h"
#include "GenericRequest.h"

using namespace cloudstorage;

class HttpServer;

class HttpCloudProvider {
 public:
  using Pointer = std::shared_ptr<HttpCloudProvider>;

  enum class Status { Accepted, Denied };

  HttpCloudProvider(HttpServer* http_server, std::string key)
      : http_server_(http_server), key_(key) {}

  ICloudProvider::Pointer provider(const IHttpServer::IConnection& connection);

  Json::Value exchange_code(const IHttpServer::IConnection& connection);

  Json::Value list_directory(const IHttpServer::IConnection& connection);

  Json::Value get_item_data(const IHttpServer::IConnection& connection);

  std::mutex& lock() const { return lock_; }

  void set_status(Status s) { status_ = s; }

 private:
  HttpServer* http_server_;
  std::string key_;
  ICloudProvider::Pointer provider_;
  std::atomic<Status> status_;
  mutable std::mutex lock_;
};

class HttpServer {
 public:
  class Callback : public ICloudProvider::ICallback {
   public:
    Callback(HttpCloudProvider* data) : data_(data) {}

    Status userConsentRequired(const ICloudProvider& p);
    void accepted(const ICloudProvider&);
    void declined(const ICloudProvider&);
    void error(const ICloudProvider&, const std::string&);

   private:
    HttpCloudProvider* data_;
  };

  class ConnectionCallback : public IHttpServer::ICallback {
   public:
    ConnectionCallback(HttpServer* server) : server_(server) {}

    IHttpServer::IResponse::Pointer receivedConnection(
        const IHttpServer&, const IHttpServer::IConnection&) override;

   private:
    HttpServer* server_;
  };

  HttpServer(Json::Value config);

  bool initialize(const std::string& provider, ICloudProvider::Hints&) const;

  Json::Value list_providers(const IHttpServer::IConnection&) const;

  HttpCloudProvider::Pointer provider(const std::string& key);

 private:
  friend class HttpCloudProvider;

  std::string auth_url_;
  uint16_t auth_port_;
  std::string file_url_;
  uint16_t daemon_port_;
  uint16_t public_daemon_port_;
  std::string youtube_dl_url_;
  Json::Value keys_;
  std::unordered_map<std::string, HttpCloudProvider::Pointer> data_;
  IHttpServerFactory::Pointer server_factory_;
  DispatchServer file_daemon_;
  IHttpServer::Pointer main_server_;
};

#endif  // HTTP_SERVER_H
