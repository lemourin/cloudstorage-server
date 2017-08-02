#ifndef UTILITY_H
#define UTILITY_H

#include <cloudstorage/ICloudProvider.h>
#include <json/json.h>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>

namespace util {

Json::Value to_json(cloudstorage::ICloudProvider::Hints);

std::string read_file(const std::string& path);

std::string to_base64(unsigned char const* bytes_to_encode,
                      unsigned int in_len);

namespace priv {
static std::mutex stream_mutex;

template <class... Args>
void log() {}

template <class First, class... Rest>
void log(First&& t, Rest&&... rest) {
  std::cerr << t << " ";
  log(std::forward<Rest>(rest)...);
}

}  // namespace priv

template <class... Args>
void log(Args&&... t) {
  std::lock_guard<std::mutex> lock(priv::stream_mutex);
  std::time_t time = std::time(nullptr);
  std::cerr << "[" << std::put_time(std::gmtime(&time), "%D %T") << "] ";
  priv::log(std::forward<Args>(t)...);
  std::cerr << std::endl;
}

}  // namespace util

#endif  // UTILITY_H
