/*****************************************************************************
 * CurlHttp.cpp : implementation of CurlHttp
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

#include "CurlHttp.h"

#include <json/json.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <sstream>

#include "Utility.h"

const uint32_t MAX_URL_LENGTH = 1024;
const uint32_t WORKER_CNT = 8;

using namespace cloudstorage;

namespace {

struct Worker {
  Worker()
      : done_(false), thread_([=] {
          while (!done_) {
            std::unique_lock<std::mutex> lock(mutex_);
            nonempty_.wait(lock, [=] { return !tasks_.empty() || done_; });
            while (!tasks_.empty()) {
              {
                auto task = tasks_.back();
                tasks_.pop_back();
                lock.unlock();
                task();
              }
              lock.lock();
            }
          }
        }) {}

  ~Worker() {
    done_ = true;
    nonempty_.notify_one();
    thread_.join();
  }

  void add(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(task);
    }
    if (!tasks_.empty()) nonempty_.notify_one();
  }

  int task_cnt() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

  std::mutex mutex_;
  std::vector<std::function<void()>> tasks_;
  std::condition_variable nonempty_;
  std::atomic_bool done_;
  std::thread thread_;
} worker[WORKER_CNT];

struct write_callback_data {
  CURL* handle_;
  std::ostream* stream_;
  std::ostream* error_stream_;
  std::shared_ptr<CurlHttpRequest::ICallback> callback_;
  bool first_call_;
  bool success_;
};

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  write_callback_data* data = static_cast<write_callback_data*>(userdata);
  if (data->first_call_) {
    data->first_call_ = false;
    if (data->callback_) {
      long http_code = 0;
      curl_easy_getinfo(data->handle_, CURLINFO_RESPONSE_CODE, &http_code);
      data->callback_->receivedHttpCode(static_cast<int>(http_code));
      data->success_ = IHttpRequest::isSuccess(static_cast<int>(http_code));
      double content_length = 0;
      curl_easy_getinfo(data->handle_, CURLINFO_CONTENT_LENGTH_DOWNLOAD,
                        &content_length);
      data->callback_->receivedContentLength(static_cast<int>(content_length));
    }
  }
  if (!data->error_stream_ || data->success_)
    data->stream_->write(ptr, static_cast<std::streamsize>(size * nmemb));
  else
    data->error_stream_->write(ptr, static_cast<std::streamsize>(size * nmemb));
  return size * nmemb;
}

size_t read_callback(char* buffer, size_t size, size_t nmemb, void* userdata) {
  std::istream* stream = static_cast<std::istream*>(userdata);
  stream->read(buffer, size * nmemb);
  return stream->gcount();
}

int progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t ultotal, curl_off_t ulnow) {
  CurlHttpRequest::ICallback* callback =
      static_cast<CurlHttpRequest::ICallback*>(clientp);
  if (callback) {
    if (ultotal != 0)
      callback->progressUpload(static_cast<uint32_t>(ultotal),
                               static_cast<uint32_t>(ulnow));
    if (dltotal != 0)
      callback->progressDownload(static_cast<uint32_t>(dltotal),
                                 static_cast<uint32_t>(dlnow));
    if (callback->abort()) return 1;
  }
  return 0;
}

std::ios::pos_type stream_length(std::istream& data) {
  data.seekg(0, data.end);
  std::ios::pos_type length = data.tellg();
  data.seekg(0, data.beg);
  return length;
}

}  // namespace

CurlHttpRequest::CurlHttpRequest(const std::string& url,
                                 const std::string& method,
                                 bool follow_redirect)
    : url_(url), method_(method), follow_redirect_(follow_redirect) {}

std::unique_ptr<CURL, CurlHttpRequest::CurlDeleter> CurlHttpRequest::init()
    const {
  std::unique_ptr<CURL, CurlDeleter> handle(curl_easy_init());
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(handle.get(), CURLOPT_READFUNCTION, read_callback);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER,
                   static_cast<long>(false));
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION,
                   static_cast<long>(follow_redirect_));
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, progress_callback);
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, static_cast<long>(false));
  return handle;
}

void CurlHttpRequest::setParameter(const std::string& parameter,
                                   const std::string& value) {
  parameters_[parameter] = value;
}

void CurlHttpRequest::setHeaderParameter(const std::string& parameter,
                                         const std::string& value) {
  header_parameters_[parameter] = value;
}

const std::unordered_map<std::string, std::string>&
CurlHttpRequest::parameters() const {
  return parameters_;
}

const std::unordered_map<std::string, std::string>&
CurlHttpRequest::headerParameters() const {
  return header_parameters_;
}

bool CurlHttpRequest::follow_redirect() const { return follow_redirect_; }

const std::string& CurlHttpRequest::url() const { return url_; }

const std::string& CurlHttpRequest::method() const { return method_; }

int CurlHttpRequest::send(std::shared_ptr<std::istream> data,
                          std::shared_ptr<std::ostream> response,
                          std::shared_ptr<std::ostream> error_stream,
                          ICallback::Pointer p) const {
  auto handle = init();
  std::shared_ptr<ICallback> callback(std::move(p));
  write_callback_data cb_data = {
      handle.get(), response.get(), error_stream.get(), callback, true, false};
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &cb_data);
  curl_slist* header_list = headerParametersToList();
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, header_list);
  std::string parameters = parametersToString();
  std::string url = url_ + (!parameters.empty() ? ("?" + parameters) : "");
  curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, callback.get());
  curl_easy_setopt(handle.get(), CURLOPT_READDATA, data.get());
  CURLcode status = CURLE_OK;
  if (method_ == "POST") {
    curl_easy_setopt(handle.get(), CURLOPT_POST, static_cast<long>(true));
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(stream_length(*data)));
  } else if (method_ == "PUT") {
    curl_easy_setopt(handle.get(), CURLOPT_UPLOAD, static_cast<long>(true));
    curl_easy_setopt(handle.get(), CURLOPT_INFILESIZE,
                     static_cast<long>(stream_length(*data)));
  } else if (method_ != "GET") {
    if (stream_length(*data) > 0)
      curl_easy_setopt(handle.get(), CURLOPT_UPLOAD, static_cast<long>(true));
    curl_easy_setopt(handle.get(), CURLOPT_CUSTOMREQUEST, method_.c_str());
  }
  status = curl_easy_perform(handle.get());
  curl_slist_free_all(header_list);
  if (status == CURLE_ABORTED_BY_CALLBACK)
    return Aborted;
  else if (status == CURLE_OK) {
    long http_code = static_cast<long>(Unknown);
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &http_code);
    if (!follow_redirect() && isRedirect(http_code)) {
      std::array<char, MAX_URL_LENGTH> redirect_url;
      char* data = redirect_url.data();
      curl_easy_getinfo(handle.get(), CURLINFO_REDIRECT_URL, &data);
      *error_stream << data;
    }
    return static_cast<int>(http_code);
  } else {
    *error_stream << curl_easy_strerror(static_cast<CURLcode>(status));
    return -status;
  }
}

void CurlHttpRequest::send(CompleteCallback c,
                           std::shared_ptr<std::istream> data,
                           std::shared_ptr<std::ostream> response,
                           std::shared_ptr<std::ostream> error_stream,
                           ICallback::Pointer cb) const {
  auto p = shared_from_this();
  auto best = &worker[0];
  for (size_t i = 1; i < WORKER_CNT; i++)
    if (best->task_cnt() > worker[i].task_cnt()) best = &worker[i];
  best->add([=]() {
    int ret = p->send(data, response, error_stream, cb);
    c(ret, response, error_stream);
  });
}

std::string CurlHttpRequest::parametersToString() const {
  std::string result;
  bool first = false;
  for (std::pair<std::string, std::string> p : parameters_) {
    if (first)
      result += "&";
    else
      first = true;
    result += p.first + "=" + p.second;
  }
  return result;
}

curl_slist* CurlHttpRequest::headerParametersToList() const {
  curl_slist* list = nullptr;
  for (std::pair<std::string, std::string> p : header_parameters_)
    list = curl_slist_append(list, (p.first + ": " + p.second).c_str());
  return list;
}

void CurlHttpRequest::CurlDeleter::operator()(CURL* handle) const {
  curl_easy_cleanup(handle);
}

IHttpRequest::Pointer CurlHttp::create(const std::string& url,
                                       const std::string& method,
                                       bool follow_redirect) const {
  return std::make_unique<CurlHttpRequest>(url, method, follow_redirect);
}