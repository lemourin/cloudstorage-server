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

#include <cassert>

const int CHUNK_SIZE = 1024;

namespace {

int http_request_callback(void* cls, MHD_Connection* c, const char* url,
                          const char* /*method*/, const char* /*version*/,
                          const char* /*upload_data*/,
                          size_t* /*upload_data_size*/, void** con_cls) {
  MicroHttpdServer* server = static_cast<MicroHttpdServer*>(cls);
  auto response = server->callback()->handle(MicroHttpdServer::Request(c, url));
  auto p = static_cast<MicroHttpdServer::Response*>(response.get());
  int ret = MHD_queue_response(c, p->code(), p->response());
  *con_cls = response.release();
  return ret;
}

void http_request_completed(void*, MHD_Connection*, void** con_cls,
                            MHD_RequestTerminationCode) {
  auto p = static_cast<MicroHttpdServer::Response*>(*con_cls);
  if (p->callback()) p->callback()();
  delete p;
}

}  // namespace

MicroHttpdServer::Response::Response(MHD_Connection* connection, int code,
                                     const IResponse::Headers& headers,
                                     int size,
                                     IResponse::ICallback::Pointer callback)
    : connection_(connection), code_(code) {
  using DataType = std::pair<MHD_Connection*, IResponse::ICallback::Pointer>;

  auto data_provider = [](void* cls, uint64_t, char* buf,
                          size_t max) -> ssize_t {
    auto data = static_cast<DataType*>(cls);
    auto r = data->second->putData(buf, max);
    if (r == IResponse::ICallback::Suspend) {
      MHD_suspend_connection(data->first);
      return 0;
    } else if (r == IResponse::ICallback::Abort)
      return MHD_CONTENT_READER_END_OF_STREAM;
    else
      return r;
  };
  auto release_data = [](void* cls) {
    auto data = static_cast<DataType*>(cls);
    delete data;
  };
  auto data = std::make_unique<DataType>(connection, std::move(callback));
  response_ = MHD_create_response_from_callback(size, CHUNK_SIZE, data_provider,
                                                data.release(), release_data);
  for (auto it : headers)
    MHD_add_response_header(response_, it.first.c_str(), it.second.c_str());
}

MicroHttpdServer::Response::~Response() {
  if (response_) MHD_destroy_response(response_);
}

void MicroHttpdServer::Response::resume() {
  MHD_resume_connection(connection_);
}

MicroHttpdServer::Request::Request(MHD_Connection* c, const char* url)
    : connection_(c), url_(url) {}

const char* MicroHttpdServer::Request::get(const std::string& name) const {
  return MHD_lookup_connection_value(connection_, MHD_GET_ARGUMENT_KIND,
                                     name.c_str());
}

const char* MicroHttpdServer::Request::header(const std::string& name) const {
  return MHD_lookup_connection_value(connection_, MHD_HEADER_KIND,
                                     name.c_str());
}

std::string MicroHttpdServer::Request::url() const { return url_; }

MicroHttpdServer::MicroHttpdServer(IHttpServer::ICallback::Pointer cb,
                                   Type type, int port, const std::string& cert,
                                   const std::string& key)
    : http_server_(create_server(type, port, http_request_callback,
                                 http_request_completed, this, cert, key)),
      callback_(cb) {}

MicroHttpdServer::IResponse::Pointer MicroHttpdServer::Request::response(
    int code, const IResponse::Headers& headers, int size,
    IResponse::ICallback::Pointer cb) const {
  return std::make_unique<Response>(connection_, code, headers, size,
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
    IHttpServer::ICallback::Pointer cb, const std::string&,
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
          ? MHD_start_daemon(
                type == MicroHttpdServer::Type::SingleThreaded
                    ? (MHD_USE_AUTO_INTERNAL_THREAD | MHD_USE_SUSPEND_RESUME)
                    : MHD_USE_THREAD_PER_CONNECTION,
                port, NULL, NULL, callback, data, MHD_OPTION_NOTIFY_COMPLETED,
                request_callback, data, MHD_OPTION_END)
          : MHD_start_daemon(
                (type == MicroHttpdServer::Type::SingleThreaded
                     ? (MHD_USE_AUTO_INTERNAL_THREAD | MHD_USE_SUSPEND_RESUME)
                     : MHD_USE_THREAD_PER_CONNECTION) |
                    MHD_USE_SSL,
                port, NULL, NULL, callback, data, MHD_OPTION_NOTIFY_COMPLETED,
                request_callback, data, MHD_OPTION_HTTPS_MEM_CERT, cert.c_str(),
                MHD_OPTION_HTTPS_MEM_KEY, key.c_str(), MHD_OPTION_END);
  assert(daemon);
  return DaemonPtr(daemon, [](MHD_Daemon* daemon) { MHD_stop_daemon(daemon); });
}
