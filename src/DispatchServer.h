#ifndef DISPATCH_SERVER_H
#define DISPATCH_SERVER_H

#include <microhttpd.h>
#include <memory>

#include "MicroHttpdServer.h"
#include "cloudstorage/IHttpServer.h"

using cloudstorage::IHttpServer;
using cloudstorage::IHttpServerFactory;

class DispatchServer {
 public:
  DispatchServer(IHttpServerFactory::Pointer, IHttpServer::Type, int port);

 private:
  friend class ServerWrapper;

  class Callback : public IHttpServer::ICallback {
   public:
    IHttpServer::IResponse::Pointer receivedConnection(
        const IHttpServer&, const IHttpServer::IConnection&) override;

    void addCallback(const std::string&, ICallback::Pointer);
    void removeCallback(const std::string&);

   private:
    std::unordered_map<std::string, ICallback::Pointer> client_callbacks_;
  };

  std::shared_ptr<Callback> callback_;
  std::shared_ptr<IHttpServer> http_server_;
};

class ServerWrapper : public IHttpServer {
 public:
  ServerWrapper(DispatchServer, const std::string& session,
                IHttpServer::ICallback::Pointer);
  ~ServerWrapper();

  IResponse::Pointer createResponse(int code, const IResponse::Headers&,
                                    const std::string& body) const override;
  IResponse::Pointer createResponse(
      int code, const IResponse::Headers&, int size, int chunk_size,
      IResponse::ICallback::Pointer) const override;

 private:
  std::string session_;
  DispatchServer server_;
};

class ServerWrapperFactory : public IHttpServerFactory {
 public:
  ServerWrapperFactory(DispatchServer);

  IHttpServer::Pointer create(IHttpServer::ICallback::Pointer,
                              const std::string& session_id, IHttpServer::Type,
                              int port) override;

 private:
  DispatchServer server_;
};

#endif  // DISPATCH_SERVER_H
