#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <cloudstorage/ICloudProvider.h>
#include <cloudstorage/ICloudStorage.h>
#include <json/json.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "DispatchServer.h"
#include "Utility.h"

using namespace cloudstorage;

class HttpServer;

struct CloudConfig {
  CloudConfig(const Json::Value&);

  std::unique_ptr<ICloudProvider::Hints> hints(
      const std::string& provider) const;

  std::string auth_url_;
  std::string file_url_;
  std::string youtube_dl_url_;
  Json::Value keys_;
  bool secure_;
};

class HttpCloudProvider {
 public:
  using Pointer = std::shared_ptr<HttpCloudProvider>;
  using Completed = std::function<void(Json::Value)>;
  using CompletedItem = std::function<void(EitherError<IItem>)>;

  enum class Status { Accepted, Denied };

  HttpCloudProvider(CloudConfig config, std::string key)
      : config_(config), key_(key) {}

  ICloudProvider::Pointer provider(HttpServer*, const IHttpServer::IRequest&);

  void item(ICloudProvider::Pointer p, HttpServer* server, const char* item_id,
            CompletedItem);

  void exchange_code(ICloudProvider::Pointer p, HttpServer* server,
                     const char* code, Completed);

  void list_directory(ICloudProvider::Pointer p, HttpServer* server,
                      const char* item_id, Completed);

  void get_item_data(ICloudProvider::Pointer p, HttpServer* server,
                     const char* item_id, Completed);

  void thumbnail(ICloudProvider::Pointer p, HttpServer* server,
                 const char* item_id, Completed);

  static Json::Value error(ICloudProvider::Pointer p, Error);

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

    IHttpServer::IResponse::Pointer handle(
        const IHttpServer::IRequest&) override;

   private:
    HttpServer* server_;
  };

  HttpServer(Json::Value config);
  ~HttpServer();

  IHttpServer::IResponse::Pointer proxy(const IHttpServer::IRequest&,
                                        const DispatchServer::Callback&);

  Json::Value list_providers(const IHttpServer::IRequest&) const;

  HttpCloudProvider::Pointer provider(const std::string& key);

  void add(std::shared_ptr<IGenericRequest>);

  int exec();

 private:
  friend class HttpCloudProvider;

  std::mutex pending_requests_mutex_;
  std::condition_variable pending_requests_condition_;
  std::vector<std::shared_ptr<IGenericRequest>> pending_requests_;
  std::atomic_bool done_;
  std::thread clean_up_thread_;
  std::unordered_map<std::string, HttpCloudProvider::Pointer> data_;
  uint16_t server_port_;
  MicroHttpdServerFactory::Pointer server_factory_;
  DispatchServer main_server_;
  ServerWrapper query_server_;
  CloudConfig config_;
  std::shared_ptr<IHttp> http_;
  util::Semaphore semaphore_;
  mutable std::mutex lock_;
};

#endif  // HTTP_SERVER_H
