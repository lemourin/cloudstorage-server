#include "DispatchServer.h"

#include "Utility.h"

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

IHttpServer::IResponse::Pointer DispatchServer::Callback::handle(
    const IHttpServer::IRequest& request) {
  if (auto ret = proxy_(request, *this)) return ret;
  const char* state = request.get("state");
  if (!state) state = "";
  auto callback = this->callback(state);
  if (!callback)
    return util::response_from_string(request, 404, {},
                                      "missing/invalid state parameter");
  else
    return callback->handle(request);
}

ServerWrapper::ServerWrapper(DispatchServer server, const std::string& session,
                             IHttpServer::ICallback::Pointer cb)
    : session_(session), server_(server) {
  server_.callback_->addCallback(session, std::move(cb));
}

ServerWrapper::~ServerWrapper() { server_.callback_->removeCallback(session_); }

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
