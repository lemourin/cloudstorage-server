#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <cloudstorage/ICloudProvider.h>
#include <cloudstorage/ICloudStorage.h>
#include <json/json.h>
#include <atomic>
#include <condition_variable>
#include <future>
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
  std::string temporary_directory_;
  Json::Value keys_;
  bool secure_;
};

class HttpCloudProvider {
 public:
  using Pointer = std::shared_ptr<HttpCloudProvider>;
  using Completed = std::function<void(Json::Value)>;
  using CompletedItem = std::function<void(EitherError<IItem>)>;

  HttpCloudProvider(CloudConfig config) : config_(config) {}

  std::shared_ptr<ICloudProvider> provider(HttpServer*,
                                           const IHttpServer::IRequest&);

  void item(std::shared_ptr<ICloudProvider> p, HttpServer* server,
            const char* item_id, CompletedItem);

  void exchange_code(std::shared_ptr<ICloudProvider> p, HttpServer* server,
                     const char* code, Completed);

  void list_directory(std::shared_ptr<ICloudProvider> p, HttpServer* server,
                      const char* item_id, const char* page_token, Completed);

  void get_item_data(std::shared_ptr<ICloudProvider> p, HttpServer* server,
                     const char* item_id, Completed);

  void thumbnail(std::shared_ptr<ICloudProvider> p, HttpServer* server,
                 const char* item_id, Completed);

  static Json::Value error(std::shared_ptr<ICloudProvider> p, Error);

 private:
  CloudConfig config_;
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

  void add(std::shared_ptr<ICloudProvider> p, std::shared_ptr<IGenericRequest>);

  int exec();

 private:
  friend class HttpCloudProvider;

  struct Request {
    std::shared_ptr<ICloudProvider> provider_;
    std::shared_ptr<IGenericRequest> request_;
  };

  std::mutex pending_requests_mutex_;
  std::condition_variable pending_requests_condition_;
  std::vector<Request> pending_requests_;
  std::atomic_bool done_;
  std::thread clean_up_thread_;
  std::atomic_int request_id_;
  uint16_t server_port_;
  std::unique_ptr<MicroHttpdServerFactory> server_factory_;
  DispatchServer main_server_;
  ServerWrapper query_server_;
  CloudConfig config_;
  std::shared_ptr<IHttp> http_;
  std::promise<int> semaphore_;
  mutable std::mutex lock_;
};

#endif  // HTTP_SERVER_H
