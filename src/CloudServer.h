#ifndef CLOUD_SERVER_H
#define CLOUD_SERVER_H

#include <microhttpd.h>

#include "cloudstorage/IHttpServer.h"

using cloudstorage::IHttpServer;
using cloudstorage::IHttpServerFactory;

class CloudServer : public IHttpServer {
 public:
  class Wrapper : public IHttpServer {
   public:
    Wrapper(CloudServer*, const std::string& session,
            IHttpServer::ICallback::Pointer);
    ~Wrapper();

    IResponse::Pointer createResponse(int code, const IResponse::Headers&,
                                      const std::string& body) const override;
    IResponse::Pointer createResponse(
        int code, const IResponse::Headers&, int size, int chunk_size,
        IResponse::ICallback::Pointer) const override;

   private:
    std::string session_;
    CloudServer* server_;
  };

  CloudServer(IHttpServer::Type type, int port);
  ~CloudServer();

  class Callback : public ICallback {
   public:
    IHttpServer::IResponse::Pointer receivedConnection(
        const IHttpServer&, const IHttpServer::IConnection&) override;
  };

  class Response : public IResponse {
   public:
    Response() = default;
    Response(int code, const IResponse::Headers&, const std::string& body);
    ~Response();

    void send(const IConnection&) override;
    int result() const { return result_; }

   protected:
    MHD_Response* response_;
    int result_;
    int code_;
  };

  class CallbackResponse : public Response {
   public:
    CallbackResponse(int code, const IResponse::Headers&, int size,
                     int chunk_size, IResponse::ICallback::Pointer);
  };

  class Connection : public IConnection {
   public:
    Connection(MHD_Connection*, const char* url);

    MHD_Connection* connection() const { return connection_; }

    const char* getParameter(const std::string& name) const override;
    const char* header(const std::string& name) const override;
    std::string url() const override;

   private:
    std::string url_;
    MHD_Connection* connection_;
  };

  IResponse::Pointer createResponse(int code, const IResponse::Headers&,
                                    const std::string& body) const override;
  IResponse::Pointer createResponse(
      int code, const IResponse::Headers&, int size, int chunk_size,
      IResponse::ICallback::Pointer) const override;

  ICallback::Pointer callback() const { return callback_; }

  void addCallback(const std::string&, ICallback::Pointer);
  void removeCallback(const std::string&);

 private:
  MHD_Daemon* http_server_;
  ICallback::Pointer callback_;
  std::unordered_map<std::string, ICallback::Pointer> client_callbacks_;
};

class ServerFactory : public IHttpServerFactory {
 public:
  ServerFactory(CloudServer* server);

  IHttpServer::Pointer create(IHttpServer::ICallback::Pointer,
                              const std::string& session_id, IHttpServer::Type,
                              int port) override;

 private:
  CloudServer* server_;
};

#endif  // CLOUD_SERVER_H
