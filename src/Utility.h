#ifndef UTILITY_H
#define UTILITY_H

#include <cloudstorage/ICloudProvider.h>
#include <json/json.h>
#include <condition_variable>
#include <memory>
#include <mutex>

class Semaphore {
 public:
  Semaphore() : count_() {}

  void notify() {
    std::unique_lock<std::mutex> lock(mutex_);
    count_++;
    condition_.notify_one();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (count_ == 0) condition_.wait(lock);
    count_--;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  uint32_t count_;
};

Json::Value to_json(cloudstorage::ICloudProvider::Hints);

std::string read_file(const std::string& path);

#endif  // UTILITY_H
