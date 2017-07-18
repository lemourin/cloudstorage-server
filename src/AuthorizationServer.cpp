#include "AuthorizationServer.h"

#include <iostream>

namespace {

int httpRequestCallback(void* cls, MHD_Connection* c, const char* url,
                        const char* /*method*/, const char* /*version*/,
                        const char* /*upload_data*/,
                        size_t* /*upload_data_size*/, void** /*ptr*/) {
  AuthorizationServer* server = static_cast<AuthorizationServer*>(cls);
  AuthorizationServer::Connection connection(c, url);
  auto response = server->callback()->receivedConnection(*server, connection);
  response->send(connection);
  return static_cast<const AuthorizationServer::Response*>(response.get())
      ->result();
}

}  // namespace

AuthorizationServer::Wrapper::Wrapper(AuthorizationServer* server,
                                      const std::string& session,
                                      IHttpServer::ICallback::Pointer cb)
    : server_(server), session_(session) {
  server_->addCallback(session, std::move(cb));
}

AuthorizationServer::Wrapper::~Wrapper() { server_->removeCallback(session_); }

IHttpServer::IResponse::Pointer AuthorizationServer::Wrapper::createResponse(
    int code, const IResponse::Headers& headers,
    const std::string& body) const {
  return server_->createResponse(code, headers, body);
}

IHttpServer::IResponse::Pointer AuthorizationServer::Wrapper::createResponse(
    int code, const IResponse::Headers& headers, int size, int chunk_size,
    IResponse::ICallback::Pointer cb) const {
  return server_->createResponse(code, headers, size, chunk_size,
                                 std::move(cb));
}

IHttpServer::IResponse::Pointer
AuthorizationServer::Callback::receivedConnection(
    const IHttpServer& d, const IHttpServer::IConnection& connection) {
  const AuthorizationServer& server =
      static_cast<const AuthorizationServer&>(d);
  const char* state = connection.getParameter("state");
  if (!state ||
      server.client_callbacks_.find(state) ==
          std::end(server.client_callbacks_))
    return d.createResponse(404, {}, "missing/invalid state parameter");
  return server.client_callbacks_.find(state)->second->receivedConnection(
      d, connection);
}

AuthorizationServer::Response::Response(int code,
                                        const IResponse::Headers& headers,
                                        const std::string& body)
    : response_(MHD_create_response_from_buffer(
          body.length(), (void*)body.c_str(), MHD_RESPMEM_MUST_COPY)),
      result_(),
      code_(code) {
  for (auto it : headers)
    MHD_add_response_header(response_, it.first.c_str(), it.second.c_str());
}

AuthorizationServer::Response::~Response() {
  if (response_) MHD_destroy_response(response_);
}

void AuthorizationServer::Response::send(const IConnection& c) {
  MHD_Connection* connection = static_cast<const Connection&>(c).connection();
  result_ = MHD_queue_response(connection, code_, response_);
}

AuthorizationServer::CallbackResponse::CallbackResponse(
    int code, const IResponse::Headers& headers, int size, int chunk_size,
    IResponse::ICallback::Pointer callback) {
  code_ = code;
  auto data_provider = [](void* cls, uint64_t, char* buf,
                          size_t max) -> ssize_t {
    auto callback = static_cast<IResponse::ICallback*>(cls);
    return callback->putData(buf, max);
  };
  auto release_data = [](void* cls) {
    auto callback = static_cast<IResponse::ICallback*>(cls);
    delete callback;
  };
  response_ = MHD_create_response_from_callback(
      size, chunk_size, data_provider, callback.release(), release_data);
  for (auto it : headers)
    MHD_add_response_header(response_, it.first.c_str(), it.second.c_str());
}

AuthorizationServer::Connection::Connection(MHD_Connection* c, const char* url)
    : connection_(c), url_(url) {}

const char* AuthorizationServer::Connection::getParameter(
    const std::string& name) const {
  return MHD_lookup_connection_value(connection_, MHD_GET_ARGUMENT_KIND,
                                     name.c_str());
}

std::string AuthorizationServer::Connection::url() const { return url_; }

AuthorizationServer::AuthorizationServer(IHttpServer::Type type, int port)
    : http_server_(MHD_start_daemon(type == IHttpServer::Type::SingleThreaded
                                        ? MHD_USE_POLL_INTERNALLY
                                        : MHD_USE_THREAD_PER_CONNECTION,
                                    port, NULL, NULL, &httpRequestCallback,
                                    this, MHD_OPTION_END)),
      callback_(std::make_unique<Callback>()) {}

AuthorizationServer::~AuthorizationServer() { MHD_stop_daemon(http_server_); }

AuthorizationServer::IResponse::Pointer AuthorizationServer::createResponse(
    int code, const IResponse::Headers& headers,
    const std::string& body) const {
  return std::make_unique<Response>(code, headers, body);
}

AuthorizationServer::IResponse::Pointer AuthorizationServer::createResponse(
    int code, const IResponse::Headers& headers, int size, int chunk_size,
    IResponse::ICallback::Pointer cb) const {
  return std::make_unique<CallbackResponse>(code, headers, size, chunk_size,
                                            std::move(cb));
}

void AuthorizationServer::addCallback(const std::string& str,
                                      ICallback::Pointer cb) {
  client_callbacks_[str] = std::move(cb);
}

void AuthorizationServer::removeCallback(const std::string& str) {
  auto it = client_callbacks_.find(str);
  if (it != std::end(client_callbacks_)) client_callbacks_.erase(it);
}

ServerFactory::ServerFactory(AuthorizationServer* server) : server_(server) {}

IHttpServer::Pointer ServerFactory::create(IHttpServer::ICallback::Pointer cb,
                                           const std::string& session,
                                           IHttpServer::Type, int port) {
  return std::make_unique<AuthorizationServer::Wrapper>(server_, session,
                                                        std::move(cb));
}
