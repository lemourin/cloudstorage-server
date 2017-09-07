#include "HttpServer.h"

extern "C" {
#include <libavutil/log.h>
}

#include <libffmpegthumbnailer/videothumbnailer.h>
#include <condition_variable>
#include <fstream>
#include <queue>
#include <sstream>

#include "Utility.h"
#include "Utility/CurlHttp.h"

using namespace std::string_literals;
using namespace std::placeholders;

const std::string SEPARATOR = "--";

namespace {

class HttpWrapper : public IHttp {
 public:
  HttpWrapper(std::shared_ptr<IHttp> p) : http_(p) {}

  IHttpRequest::Pointer create(const std::string& url,
                               const std::string& method,
                               bool follow_redirect) const override {
    return http_->create(url, method, follow_redirect);
  }

 private:
  std::shared_ptr<IHttp> http_;
};

std::string file_type_to_string(IItem::FileType type) {
  switch (type) {
    case IItem::FileType::Audio:
      return "audio";
    case IItem::FileType::Directory:
      return "directory";
    case IItem::FileType::Image:
      return "image";
    case IItem::FileType::Unknown:
      return "unknown";
    case IItem::FileType::Video:
      return "video";
    default:
      return "invalid";
  }
}

Json::Value session(std::shared_ptr<ICloudProvider> p) {
  Json::Value result;
  result["token"] = p->token();
  if (!p->hints()["access_token"].empty())
    result["access_token"] = p->hints()["access_token"];
  result["provider"] = p->name();
  return result;
}

struct Buffer {
  void resume() {
    if (suspended_ && response_) {
      suspended_ = false;
      response_->resume();
    }
  }

  std::mutex lock_;
  std::stringstream result_;
  bool ready_ = false;
  bool suspended_ = false;
  IHttpServer::IResponse* response_;
};

class ResponseCallback : public IHttpServer::IResponse::ICallback {
 public:
  ResponseCallback(std::shared_ptr<Buffer> p) : buffer_(p) {}

  int putData(char* buffer, size_t size) override {
    std::lock_guard<std::mutex> lock(buffer_->lock_);
    if (!buffer_->ready_) {
      buffer_->suspended_ = true;
      return Suspend;
    }
    auto r = buffer_->result_.readsome(buffer, size);
    if (r == 0) return Abort;
    return r;
  }

 private:
  std::shared_ptr<Buffer> buffer_;
};

}  // namespace

CloudConfig::CloudConfig(const Json::Value& config)
    : auth_url_(config["auth_url"].asString()),
      file_url_(config["file_url"].asString()),
      youtube_dl_url_(config["youtube_dl_url"].asString()),
      temporary_directory_(config["temporary_directory"].asString()),
      keys_(config["keys"]),
      secure_(!config["ssl_key"].empty()) {}

std::unique_ptr<ICloudProvider::Hints> CloudConfig::hints(
    const std::string& provider) const {
  if (!keys_.isMember(provider)) return nullptr;
  auto p = std::make_unique<ICloudProvider::Hints>();
  auto& hints = *p;
  hints["youtube_dl_url"] = youtube_dl_url_;
  hints["client_id"] = keys_[provider]["client_id"].asString();
  hints["client_secret"] = keys_[provider]["client_secret"].asString();
  hints["redirect_uri"] = auth_url_;
  hints["temporary_directory"] = temporary_directory_;
  hints["file_url"] = file_url_ + "/" + provider;
  return p;
}

IHttpServer::IResponse::Pointer HttpServer::ConnectionCallback::handle(
    const IHttpServer::IRequest& c) {
  Json::Value result(Json::objectValue);
  if (server_->done_)
    return util::response_from_string(c, IHttpRequest::ServiceUnavailable, {},
                                      "");
  if (c.url() == "/quit") {
    server_->semaphore_.notify();
  } else {
    const char* provider = c.get("provider");
    if (provider) {
      HttpCloudProvider p(server_->config_);
      auto r = p.provider(server_, c);
      if (!r) {
        result["error"] = "invalid provider";
      } else {
        auto start_time = std::chrono::system_clock::now();
        auto buffer = std::make_shared<Buffer>();
        auto cb = std::make_unique<ResponseCallback>(buffer);
        auto url = c.url();
        auto response =
            c.response(IHttpRequest::Ok, {{"Content-Type", "application/json"}},
                       -1, std::move(cb));
        buffer->response_ = response.get();
        response->completed([=]() {
          std::lock_guard<std::mutex> lock(buffer->lock_);
          buffer->response_ = nullptr;
        });
        auto func = [=](auto e) {
          std::lock_guard<std::mutex> lock(buffer->lock_);
          buffer->result_ << Json::StyledWriter().write(e);
          buffer->ready_ = true;
          buffer->resume();
          util::log(url, "lasted",
                    std::chrono::duration<double>(
                        std::chrono::system_clock::now() - start_time)
                        .count());
        };
        if (c.url() == "/exchange_code"s) {
          p.exchange_code(r, server_, c.get("code"), func);
        } else if (c.url() == "/list_directory"s) {
          p.list_directory(r, server_, c.get("item_id"), c.get("page_token"),
                           func);
        } else if (c.url() == "/get_item_data"s) {
          p.get_item_data(r, server_, c.get("item_id"), func);
        } else if (c.url() == "/thumbnail"s) {
          p.thumbnail(r, server_, c.get("item_id"), func);
        } else {
          result["error"] = "bad request";
          func(result);
        }
        return response;
      }
    } else {
      if (c.url() == "/list_providers"s)
        result = server_->list_providers(c);
      else
        result["error"] = "invalid request";
    }
  }

  if (c.url() != "/health_check") util::log(c.url(), "received");

  auto str = Json::StyledWriter().write(result);
  return util::response_from_string(
      c, 200, {{"Content-Type", "application/json"}}, str);
}

HttpServer::HttpServer(Json::Value config)
    : done_(),
      clean_up_thread_([=]() {
        while (!done_) {
          std::unique_lock<std::mutex> lock(pending_requests_mutex_);
          pending_requests_condition_.wait(
              lock, [=]() { return !pending_requests_.empty() || done_; });
          while (!pending_requests_.empty()) {
            auto r = pending_requests_.back();
            pending_requests_.pop_back();
            lock.unlock();
            r.request_->finish();
            r.provider_ = nullptr;
            lock.lock();
          }
        }
      }),
      request_id_(),
      server_port_(config["port"].asInt()),
      server_factory_(std::make_unique<MicroHttpdServerFactory>()),
      main_server_(DispatchServer(server_factory_.get(), server_port_,
                                  std::bind(&HttpServer::proxy, this, _1, _2))),
      query_server_(main_server_, "",
                    std::make_unique<ConnectionCallback>(this)),
      config_(config),
      http_(std::make_shared<curl::CurlHttp>()) {
  av_log_set_level(AV_LOG_PANIC);
}

HttpServer::~HttpServer() {
  done_ = true;
  pending_requests_condition_.notify_one();
  clean_up_thread_.join();
}

IHttpServer::IResponse::Pointer HttpServer::proxy(
    const IHttpServer::IRequest& request,
    const DispatchServer::Callback& callback) {
  if (request.url() == "/files") {
    auto provider = request.get("provider");
    auto file = request.get("file");
    if (!provider || !file)
      return util::response_from_string(request, IHttpRequest::Bad, {}, "");
    HttpCloudProvider c(config_);
    auto provider_object = c.provider(this, request);
    if (!provider_object) return nullptr;
    auto state = provider_object->hints()["state"];
    auto f = callback.callback(state);
    if (!f) return nullptr;
    class Wrapper : public RequestWrapper {
     public:
      Wrapper(const IHttpServer::IRequest& r, std::string state)
          : RequestWrapper(r), state_(state) {}

      const char* get(const std::string& name) const override {
        if (name == "state") return state_.c_str();
        return RequestWrapper::get(name);
      }

     private:
      std::string state_;
    };
    auto r = f->handle(Wrapper(request, state));
    r->completed([=] { (void)provider_object; });
    return r;
  }
  return nullptr;
}

ICloudProvider::IAuthCallback::Status
HttpServer::AuthCallback::userConsentRequired(const ICloudProvider& p) {
  util::log("waiting for user consent", p.name());
  return Status::None;
}

void HttpServer::AuthCallback::done(const ICloudProvider& p,
                                    EitherError<void> e) {
  if (e.left())
    util::log("auth error", e.left()->code_, e.left()->description_);
  else
    util::log("accepted", p.name(), p.token());
}

std::shared_ptr<ICloudProvider> HttpCloudProvider::provider(
    HttpServer* server, const IHttpServer::IRequest& request) {
  const char* provider = request.get("provider");
  const char* token = request.get("token");
  const char* access_token = request.get("access_token");
  if (!provider || !token) return nullptr;
  auto hints = config_.hints(provider);
  if (!hints) return nullptr;
  if (access_token) (*hints)["access_token"] = access_token;
  (*hints)["state"] =
      provider + SEPARATOR + std::to_string(server->request_id_++);
  ICloudProvider::InitData data;
  data.token_ = token;
  data.http_server_ =
      std::make_unique<ServerWrapperFactory>(server->main_server_);
  data.http_engine_ = std::make_unique<HttpWrapper>(server->http_);
  data.hints_ = *hints;
  data.callback_ = std::make_unique<HttpServer::AuthCallback>(this);
  return ICloudStorage::create()->provider(provider, std::move(data));
}

void HttpCloudProvider::exchange_code(std::shared_ptr<ICloudProvider> p,
                                      HttpServer* server, const char* code,
                                      Completed c) {
  if (!code) {
    Json::Value result;
    result["error"] = "missing code";
    return c(result);
  }
  server->add(p, p->exchangeCodeAsync(code, [=](auto token) {
    if (token.right()) {
      Json::Value result;
      result["token"] = token.right()->token_;
      result["access_token"] = token.right()->access_token_;
      result["provider"] = p->name();
      c(result);
    } else {
      c(error(p, *token.left()));
    }
  }));
}

void HttpCloudProvider::list_directory(std::shared_ptr<ICloudProvider> p,
                                       HttpServer* server, const char* item_id,
                                       const char* page_token, Completed c) {
  if (!page_token)
    return c(error(p, Error{IHttpRequest::Bad, "missing page token"}));
  item(p, server, item_id, [=](auto item) {
    if (item.left()) return c(error(p, *item.left()));
    server->add(
        p, p->listDirectoryPageAsync(item.right(), page_token, [=](auto list) {
          if (list.right()) {
            Json::Value result = session(p);
            Json::Value array(Json::arrayValue);
            auto lst = list.right()->items_;
            for (auto i : lst) {
              Json::Value v;
              v["id"] = i->id();
              v["filename"] = i->filename();
              v["type"] = file_type_to_string(i->type());
              array.append(v);
            }
            result["items"] = array;
            if (!list.right()->next_token_.empty())
              result["next_token"] = list.right()->next_token_;
            c(result);
          } else {
            c(error(p, *list.left()));
          }
        }));
  });
}

void HttpCloudProvider::get_item_data(std::shared_ptr<ICloudProvider> p,
                                      HttpServer* server, const char* item_id,
                                      Completed c) {
  item(p, server, item_id, [=](auto item) {
    if (item.left()) {
      c(error(p, *item.left()));
    } else {
      Json::Value result = session(p);
      result["url"] = item.right()->url();
      result["id"] = item.right()->id();
      c(result);
    }
  });
}

void HttpCloudProvider::item(std::shared_ptr<ICloudProvider> p,
                             HttpServer* server, const char* item_id,
                             CompletedItem c) {
  if (!item_id)
    c(Error{IHttpRequest::NotFound, "not found"});
  else
    item_id == "root"s ? c(p->rootDirectory())
                       : server->add(p, p->getItemDataAsync(item_id, c));
}

void HttpCloudProvider::thumbnail(std::shared_ptr<ICloudProvider> p,
                                  HttpServer* server, const char* item_id,
                                  Completed c) {
  item(p, server, item_id, [=](auto item) {
    if (item.left()) return c(error(p, *item.left()));

    class download : public IDownloadFileCallback {
     public:
      download(EitherError<IItem> item, std::shared_ptr<ICloudProvider> p,
               bool secure, uint16_t port, Completed c)
          : item_(item), p_(p), secure_(secure), port_(port), c_(c) {}

      void receivedData(const char* data, uint32_t length) override {
        for (size_t i = 0; i < length; i++) data_.push_back(data[i]);
      }
      void done(EitherError<void> thumbnail) override {
        auto i = item_.right();
        auto c = std::move(c_);
        auto p = std::move(p_);
        auto secure = secure_;
        auto port = port_;
        auto f = [=](const std::vector<char>& data) {
          Json::Value result = session(p);
          result["thumbnail"] = util::to_base64(
              reinterpret_cast<const unsigned char*>(data.begin().base()),
              data.size());
          c(result);
        };
        if (thumbnail.left()) {
          util::enqueue([=]() {
            if ((i->type() == IItem::FileType::Video ||
                 i->type() == IItem::FileType::Image) &&
                !i->url().empty()) {
              try {
                std::vector<uint8_t> buffer;
                ffmpegthumbnailer::VideoThumbnailer thumbnailer;
                auto file_url = p->hints()["file_url"];
                auto url = i->url();
                if (!file_url.empty() && url.length() >= file_url.length()) {
                  if (url.substr(0, file_url.length()) == file_url) {
                    auto rest =
                        std::string(url.begin() + file_url.length(), url.end());
                    url = (secure ? "https" : "http") + "://127.0.0.1:"s +
                          std::to_string(port) + rest;
                  }
                }
                thumbnailer.generateThumbnail(url, ThumbnailerImageType::Png,
                                              buffer);
                auto ptr = reinterpret_cast<const char*>(buffer.data());
                f(std::vector<char>(ptr, ptr + buffer.size()));
              } catch (const std::exception& e) {
                util::log("couldn't generate thumbnail:", e.what());
                c(error(p, Error{IHttpRequest::Bad, e.what()}));
              }
            } else {
              c(error(
                  p, Error{IHttpRequest::Bad, "couldn't generate thumbnail"s}));
            }
          });
        } else {
          f(data_);
        }
      }
      void progress(uint32_t, uint32_t) override {}

      EitherError<IItem> item_;
      std::shared_ptr<ICloudProvider> p_;
      bool secure_;
      uint16_t port_;
      Completed c_;
      std::vector<char> data_;
    };

    server->add(
        p,
        p->getThumbnailAsync(item.right(), std::make_shared<download>(
                                               item, p, server->config_.secure_,
                                               server->server_port_, c)));
  });
}

Json::Value HttpCloudProvider::error(std::shared_ptr<ICloudProvider> p,
                                     Error e) {
  Json::Value result;
  result["error"] = e.code_;
  result["error_description"] = e.description_;
  result["consent_url"] = p->authorizeLibraryUrl();
  return result;
}

Json::Value HttpServer::list_providers(const IHttpServer::IRequest&) const {
  Json::Value result;
  Json::Value array(Json::arrayValue);
  for (auto t : ICloudStorage::create()->providers()) {
    if (auto hints = config_.hints(t)) {
      ICloudProvider::InitData data;
      data.hints_ = *hints;
      data.hints_["state"] = t + SEPARATOR;
      data.http_server_ = std::make_unique<MicroHttpdServerFactory>();
      data.http_engine_ = std::make_unique<HttpWrapper>(http_);
      auto p = ICloudStorage::create()->provider(t, std::move(data));
      Json::Value v;
      v["name"] = p->name();
      v["url"] = p->authorizeLibraryUrl();
      array.append(v);
    }
  }
  result["providers"] = array;
  return result;
}

void HttpServer::add(std::shared_ptr<ICloudProvider> p,
                     std::shared_ptr<IGenericRequest> r) {
  {
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    pending_requests_.push_back({p, r});
  }
  pending_requests_condition_.notify_one();
}

int HttpServer::exec() {
  semaphore_.wait();
  return 0;
}
