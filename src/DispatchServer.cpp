#include "DispatchServer.h"

#include <iostream>

DispatchServer::DispatchServer(MicroHttpdServerFactory::Pointer f,
                               uint16_t port, ProxyFunction p)
    : callback_(std::make_shared<Callback>(p)),
      http_server_(f->create(callback_, "",
                             MicroHttpdServer::Type::SingleThreaded, port)) {}

DispatchServer::Callback::Callback(ProxyFunction f) : proxy_(f) {}

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

IHttpServer::ICallback::Pointer DispatchServer::Callback::callback(
    const std::string& str) const {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = client_callbacks_.find(str);
  return it == std::end(client_callbacks_) ? nullptr : it->second;
}

IHttpServer::IResponse::Pointer DispatchServer::Callback::receivedConnection(
    const IHttpServer& server, IHttpServer::IConnection::Pointer connection) {
  if (auto ret = proxy_(server, connection, *this)) return ret;
  const char* state = connection->getParameter("state");
  if (!state) state = "";
  auto callback = this->callback(state);
  if (!callback)
    return server.createResponse(404, {}, "missing/invalid state parameter");
  else
    return callback->receivedConnection(server, connection);
}

ServerWrapper::ServerWrapper(DispatchServer server, const std::string& session,
                             IHttpServer::ICallback::Pointer cb)
    : session_(session), server_(server) {
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

IHttpServer::ICallback::Pointer ServerWrapper::callback() const {
  return server_.http_server_->callback();
}

ServerWrapperFactory::ServerWrapperFactory(DispatchServer server)
    : server_(server) {}

IHttpServer::Pointer ServerWrapperFactory::create(
    IHttpServer::ICallback::Pointer cb, const std::string& session,
    IHttpServer::Type) {
  return std::make_unique<ServerWrapper>(server_, session, std::move(cb));
}
