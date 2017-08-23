#include "Utility.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

const std::string base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

const int WORKER_CNT = 1;

namespace util {

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

}  // namespace

void enqueue(std::function<void()> f) {
  auto best = &worker[0];
  for (size_t i = 1; i < WORKER_CNT; i++)
    if (best->task_cnt() > worker[i].task_cnt()) best = &worker[i];
  best->add(f);
}

Json::Value to_json(cloudstorage::ICloudProvider::Hints h) {
  Json::Value result;
  for (auto it : h) result[it.first] = it.second;
  return result;
}

std::string read_file(const std::string& path) {
  std::fstream f(path);
  std::stringstream stream;
  stream << f.rdbuf();
  return stream.str();
}

std::string to_base64(const unsigned char* bytes_to_encode,
                      unsigned int in_len) {
  std::string ret;
  int i = 0;
  int j = 0;
  unsigned char char_array_3[3];
  unsigned char char_array_4[4];

  while (in_len--) {
    char_array_3[i++] = *(bytes_to_encode++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] =
          ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] =
          ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for (i = 0; (i < 4); i++) ret += base64_chars[char_array_4[i]];
      i = 0;
    }
  }

  if (i) {
    for (j = i; j < 3; j++) char_array_3[j] = '\0';

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] =
        ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] =
        ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

    for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];

    while ((i++ < 3)) ret += '=';
  }

  return ret;
}

cloudstorage::IHttpServer::IResponse::Pointer response_from_string(
    const cloudstorage::IHttpServer::IRequest& request, int code,
    const cloudstorage::IHttpServer::IResponse::Headers& headers,
    const std::string& data) {
  class DataProvider : public cloudstorage::IHttpServer::IResponse::ICallback {
   public:
    DataProvider(const std::string& data) : position_(), data_(data) {}

    int putData(char* buffer, size_t max) override {
      int cnt = std::min(data_.length() - position_, max);
      memcpy(buffer, (data_.begin() + position_).base(), cnt);
      position_ += cnt;
      return cnt;
    }

   private:
    int position_;
    std::string data_;
  };
  return request.response(code, headers, data.length(),
                          std::make_unique<DataProvider>(data));
}

}  // namespace util
