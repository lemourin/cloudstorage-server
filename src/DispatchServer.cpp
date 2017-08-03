#include "DispatchServer.h"

#include <iostream>

DispatchServer::DispatchServer(MicroHttpdServerFactory::Pointer f,
                               uint16_t port)
    : callback_(std::make_shared<Callback>()),
      http_server_(f->create(callback_, "",
                             MicroHttpdServer::Type::SingleThreaded, port)) {}

void DispatchServer::Callback::addCallback(const std::string& str,
                                           ICallback::Pointer cb) {
  std::lock_guard<std::mutex> lock(lock_);
  client_callbacks_[str] = std::move(cb);
}

void DispatchServer::Callback::removeCallback(const std::string& str) {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = client_callbacks_.find(str);
  if (it != std::end(client_callbacks_)) client_callbacks_.erase(it);
}

IHttpServer::IResponse::Pointer DispatchServer::Callback::receivedConnection(
    const IHttpServer& server, IHttpServer::IConnection::Pointer connection) {
  const char* state = connection->getParameter("state");
  if (!state || client_callbacks_.find(state) == std::end(client_callbacks_))
    return server.createResponse(404, {}, "missing/invalid state parameter");
  return client_callbacks_.find(state)->second->receivedConnection(server,
                                                                   connection);
}

ServerWrapper::ServerWrapper(DispatchServer server, const std::string& session,
                             IHttpServer::ICallback::Pointer cb)
    : server_(server), session_(session) {
  server_.callback_->addCallback(session, std::move(cb));
}

ServerWrapper::~ServerWrapper() { server_.callback_->removeCallback(session_); }

IHttpServer::IResponse::Pointer ServerWrapper::createResponse(
    int code, const IResponse::Headers& headers,
    const std::string& body) const {
  return server_.http_server_->createResponse(code, headers, body);
}

IHttpServer::IResponse::Pointer ServerWrapper::createResponse(
    int code, const IResponse::Headers& headers, int size, int chunk_size,
    IResponse::ICallback::Pointer cb) const {
  return server_.http_server_->createResponse(code, headers, size, chunk_size,
                                              std::move(cb));
}

ServerWrapperFactory::ServerWrapperFactory(DispatchServer server)
    : server_(server) {}

IHttpServer::Pointer ServerWrapperFactory::create(
    IHttpServer::ICallback::Pointer cb, const std::string& session,
    IHttpServer::Type) {
  return std::make_unique<ServerWrapper>(server_, session, std::move(cb));
}
