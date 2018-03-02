#ifndef DISPATCH_SERVER_H
#define DISPATCH_SERVER_H

#define WITH_MICROHTTPD

#include <memory>
#include <mutex>

#include "Utility/MicroHttpdServer.h"
#include "cloudstorage/IHttpServer.h"

using cloudstorage::IHttpServer;
using cloudstorage::IHttpServerFactory;
using cloudstorage::MicroHttpdServerFactory;

class DispatchServer {
 public:
  class Callback;

  using ProxyFunction = std::function<IHttpServer::IResponse::Pointer(
      const IHttpServer::IRequest&, const Callback&)>;

  class Callback : public IHttpServer::ICallback {
   public:
    Callback(ProxyFunction);

    IHttpServer::IResponse::Pointer handle(
        const IHttpServer::IRequest&) override;

    void addCallback(const std::string&, ICallback::Pointer);
    void removeCallback(const std::string&);
    ICallback::Pointer callback(const std::string&) const;

   private:
    ProxyFunction proxy_;
    std::unordered_map<std::string, ICallback::Pointer> client_callbacks_;
    mutable std::mutex lock_;
  };

  DispatchServer(MicroHttpdServerFactory*, uint16_t port, ProxyFunction);

 private:
  friend class ServerWrapper;

  std::shared_ptr<Callback> callback_;
  std::shared_ptr<IHttpServer> http_server_;
};

class ServerWrapper : public IHttpServer {
 public:
  ServerWrapper(DispatchServer, const std::string& session,
                IHttpServer::ICallback::Pointer);
  ~ServerWrapper();

  ICallback::Pointer callback() const override;

 private:
  std::string session_;
  DispatchServer server_;
};

class ServerWrapperFactory : public IHttpServerFactory {
 public:
  ServerWrapperFactory(DispatchServer);

  IHttpServer::Pointer create(IHttpServer::ICallback::Pointer,
                              const std::string& session_id,
                              IHttpServer::Type) override;

 private:
  DispatchServer server_;
};

class RequestWrapper : public IHttpServer::IRequest {
 public:
  RequestWrapper(const IHttpServer::IRequest& r) : r_(r) {}

  const char* get(const std::string& name) const override {
    return r_.get(name);
  }

  const char* header(const std::string& name) const override {
    return r_.header(name);
  }

  std::string url() const override { return r_.url(); }

  std::string method() const override { return r_.method(); }

  IHttpServer::IResponse::Pointer response(
      int code, const IHttpServer::IResponse::Headers& h, int size,
      IHttpServer::IResponse::ICallback::Pointer cb) const override {
    return r_.response(code, h, size, std::move(cb));
  }

 private:
  const IHttpServer::IRequest& r_;
};

#endif  // DISPATCH_SERVER_H
