/*****************************************************************************
 * MicroHttpdServer.cpp : implementation of MicroHttpdServer
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "MicroHttpdServer.h"

namespace {

int http_request_callback(void* cls, MHD_Connection* c, const char* url,
                          const char* /*method*/, const char* /*version*/,
                          const char* /*upload_data*/,
                          size_t* /*upload_data_size*/, void** con_cls) {
  MicroHttpdServer* server = static_cast<MicroHttpdServer*>(cls);
  auto connection = std::make_unique<MicroHttpdServer::Connection>(c, url);
  auto response =
      server->callback()->receivedConnection(*server, connection.get());
  auto p = static_cast<MicroHttpdServer::Response*>(response.get());
  int ret = MHD_queue_response(c, p->code(), p->response());
  *con_cls = connection.release();
  return ret;
}

void http_request_completed(void*, MHD_Connection*, void** con_cls,
                            MHD_RequestTerminationCode) {
  auto p = static_cast<MicroHttpdServer::Connection*>(*con_cls);
  if (p->callback()) p->callback()();
  delete p;
}

}  // namespace

MicroHttpdServer::Response::Response(int code,
                                     const IResponse::Headers& headers,
                                     const std::string& body)
    : response_(MHD_create_response_from_buffer(
          body.length(), (void*)body.c_str(), MHD_RESPMEM_MUST_COPY)),
      code_(code) {
  for (auto it : headers)
    MHD_add_response_header(response_, it.first.c_str(), it.second.c_str());
}

MicroHttpdServer::Response::~Response() {
  if (response_) MHD_destroy_response(response_);
}

MicroHttpdServer::CallbackResponse::CallbackResponse(
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

MicroHttpdServer::Connection::Connection(MHD_Connection* c, const char* url)
    : connection_(c), url_(url) {}

const char* MicroHttpdServer::Connection::getParameter(
    const std::string& name) const {
  return MHD_lookup_connection_value(connection_, MHD_GET_ARGUMENT_KIND,
                                     name.c_str());
}

const char* MicroHttpdServer::Connection::header(
    const std::string& name) const {
  return MHD_lookup_connection_value(connection_, MHD_HEADER_KIND,
                                     name.c_str());
}

std::string MicroHttpdServer::Connection::url() const { return url_; }

void MicroHttpdServer::Connection::onCompleted(CompletedCallback f) {
  callback_ = f;
}

void MicroHttpdServer::Connection::suspend() {
  MHD_suspend_connection(connection_);
}

void MicroHttpdServer::Connection::resume() {
  MHD_resume_connection(connection_);
}

MicroHttpdServer::MicroHttpdServer(IHttpServer::ICallback::Pointer cb,
                                   Type type, int port, const std::string& cert,
                                   const std::string& key)
    : http_server_(create_server(type, port, http_request_callback,
                                 http_request_completed, this, cert, key)),
      callback_(cb) {}

MicroHttpdServer::IResponse::Pointer MicroHttpdServer::createResponse(
    int code, const IResponse::Headers& headers,
    const std::string& body) const {
  return std::make_unique<Response>(code, headers, body);
}

MicroHttpdServer::IResponse::Pointer MicroHttpdServer::createResponse(
    int code, const IResponse::Headers& headers, int size, int chunk_size,
    IResponse::ICallback::Pointer cb) const {
  return std::make_unique<CallbackResponse>(code, headers, size, chunk_size,
                                            std::move(cb));
}

MicroHttpdServerFactory::MicroHttpdServerFactory(const std::string& cert,
                                                 const std::string& key)
    : cert_(cert), key_(key) {}

IHttpServer::Pointer MicroHttpdServerFactory::create(
    IHttpServer::ICallback::Pointer, const std::string&, IHttpServer::Type) {
  return nullptr;
}

IHttpServer::Pointer MicroHttpdServerFactory::create(
    IHttpServer::ICallback::Pointer cb, const std::string& session_id,
    MicroHttpdServer::Type type, uint16_t port) {
  return std::make_unique<MicroHttpdServer>(cb, type, port, cert_, key_);
}

DaemonPtr create_server(MicroHttpdServer::Type type, int port,
                        MHD_AccessHandlerCallback callback,
                        MHD_RequestCompletedCallback request_callback,
                        void* data, const std::string& cert,
                        const std::string& key) {
  MHD_Daemon* daemon =
      cert.empty()
          ? MHD_start_daemon(type == MicroHttpdServer::Type::SingleThreaded
                                 ? (MHD_USE_EPOLL_INTERNALLY_LINUX_ONLY |
                                    MHD_USE_SUSPEND_RESUME)
                                 : MHD_USE_THREAD_PER_CONNECTION,
                             port, NULL, NULL, callback, data,
                             MHD_OPTION_NOTIFY_COMPLETED,
                             http_request_completed, data, MHD_OPTION_END)
          : MHD_start_daemon(
                (type == MicroHttpdServer::Type::SingleThreaded
                     ? (MHD_USE_EPOLL_INTERNALLY_LINUX_ONLY |
                        MHD_USE_SUSPEND_RESUME)
                     : MHD_USE_THREAD_PER_CONNECTION) |
                    MHD_USE_SSL,
                port, NULL, NULL, callback, data, MHD_OPTION_NOTIFY_COMPLETED,
                http_request_completed, data, MHD_OPTION_HTTPS_MEM_CERT,
                cert.c_str(), MHD_OPTION_HTTPS_MEM_KEY, key.c_str(),
                MHD_OPTION_END);
  return DaemonPtr(daemon, [](MHD_Daemon* daemon) { MHD_stop_daemon(daemon); });
}
