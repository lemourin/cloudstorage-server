#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <cloudstorage/ICloudProvider.h>
#include <cloudstorage/ICloudStorage.h>
#include <json/json.h>
#include <atomic>
#include <mutex>

#include "DispatchServer.h"

using namespace cloudstorage;

class HttpServer;

struct CloudConfig {
  CloudConfig(const Json::Value&, IHttpServerFactory::Pointer);

  std::unique_ptr<ICloudProvider::Hints> hints(
      const std::string& provider) const;

  std::string auth_url_;
  uint16_t auth_port_;
  std::string file_url_;
  uint16_t daemon_port_;
  uint16_t public_daemon_port_;
  std::string youtube_dl_url_;
  Json::Value keys_;
  DispatchServer file_daemon_;
};

class HttpCloudProvider {
 public:
  using Pointer = std::shared_ptr<HttpCloudProvider>;

  enum class Status { Accepted, Denied };

  HttpCloudProvider(CloudConfig config, std::string key)
      : config_(config), key_(key) {}

  ICloudProvider::Pointer provider(const IHttpServer::IConnection& connection);

  EitherError<IItem> item(ICloudProvider::Pointer p,
                          const IHttpServer::IConnection& connection);

  Json::Value exchange_code(ICloudProvider::Pointer p,
                            const IHttpServer::IConnection& connection);

  Json::Value list_directory(ICloudProvider::Pointer p,
                             const IHttpServer::IConnection& connection);

  Json::Value get_item_data(ICloudProvider::Pointer p,
                            const IHttpServer::IConnection& connection);

  Json::Value thumbnail(ICloudProvider::Pointer p,
                        const IHttpServer::IConnection& connection);

  Json::Value error(ICloudProvider::Pointer p, Error) const;

  std::mutex& lock() const { return lock_; }

  void set_status(Status s) { status_ = s; }

 private:
  CloudConfig config_;
  std::string key_;
  ICloudProvider::Pointer provider_;
  std::atomic<Status> status_;
  mutable std::mutex lock_;
};

class HttpServer {
 public:
  class AuthCallback : public ICloudProvider::IAuthCallback {
   public:
    AuthCallback(HttpCloudProvider* data) : data_(data) {}

    Status userConsentRequired(const ICloudProvider& p) override;
    void done(const ICloudProvider&, EitherError<void>) override;

   private:
    HttpCloudProvider* data_;
  };

  class ConnectionCallback : public IHttpServer::ICallback {
   public:
    ConnectionCallback(HttpServer* server) : server_(server) {}

    IHttpServer::IResponse::Pointer receivedConnection(
        const IHttpServer&, IHttpServer::IConnection::Pointer) override;

   private:
    HttpServer* server_;
  };

  HttpServer(Json::Value config);

  Json::Value list_providers(const IHttpServer::IConnection&) const;

  HttpCloudProvider::Pointer provider(const std::string& key);

 private:
  friend class HttpCloudProvider;

  std::unordered_map<std::string, HttpCloudProvider::Pointer> data_;
  MicroHttpdServerFactory::Pointer server_factory_;
  IHttpServer::Pointer main_server_;
  CloudConfig config_;
  mutable std::mutex lock_;
};

#endif  // HTTP_SERVER_H
