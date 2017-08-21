/*****************************************************************************
 * CurlHttp.h : interface of CurlHttp
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

#ifndef CURLHTTP_H
#define CURLHTTP_H

#include <curl/curl.h>
#include <memory>
#include <string>
#include <unordered_map>

#include <cloudstorage/IHttp.h>

using cloudstorage::IHttp;
using cloudstorage::IHttpRequest;

class CurlHttp : public IHttp {
 public:
  IHttpRequest::Pointer create(const std::string&, const std::string&,
                               bool) const override;
};

class CurlHttpRequest : public IHttpRequest,
                        public std::enable_shared_from_this<CurlHttpRequest> {
 public:
  struct CurlDeleter {
    void operator()(CURL*) const;
  };

  struct CurlListDeleter {
    void operator()(curl_slist*) const;
  };

  struct RequestData {
    using Pointer = std::unique_ptr<RequestData>;

    std::unique_ptr<CURL, CurlDeleter> handle_;
    std::unique_ptr<curl_slist, CurlListDeleter> headers_;
    std::shared_ptr<std::istream> data_;
    std::shared_ptr<std::ostream> stream_;
    std::shared_ptr<std::ostream> error_stream_;
    std::shared_ptr<CurlHttpRequest::ICallback> callback_;
    CompleteCallback complete_;
    bool follow_redirect_;
    bool first_call_;
    bool success_;

    void done(int result);
  };

  CurlHttpRequest(const std::string& url, const std::string& method,
                  bool follow_redirect);
  std::unique_ptr<CURL, CurlDeleter> init() const;

  void setParameter(const std::string& parameter,
                    const std::string& value) override;
  void setHeaderParameter(const std::string& parameter,
                          const std::string& value) override;

  const std::unordered_map<std::string, std::string>& parameters()
      const override;
  const std::unordered_map<std::string, std::string>& headerParameters()
      const override;

  const std::string& url() const override;
  const std::string& method() const override;
  bool follow_redirect() const override;

  RequestData::Pointer prepare(CompleteCallback,
                               std::shared_ptr<std::istream> data,
                               std::shared_ptr<std::ostream> response,
                               std::shared_ptr<std::ostream> error_stream,
                               ICallback::Pointer = nullptr) const;

  void send(CompleteCallback, std::shared_ptr<std::istream> data,
            std::shared_ptr<std::ostream> response,
            std::shared_ptr<std::ostream> error_stream,
            ICallback::Pointer = nullptr) const override;

  std::string parametersToString() const;

 private:
  std::unique_ptr<curl_slist, CurlListDeleter> headerParametersToList() const;

  std::string url_;
  std::unordered_map<std::string, std::string> parameters_;
  std::unordered_map<std::string, std::string> header_parameters_;
  std::string method_;
  bool follow_redirect_;
};

#endif  // CURLHTTP_H
