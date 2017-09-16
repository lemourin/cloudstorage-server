#ifndef HTTP_UTILITY_H
#define HTTP_UTILITY_H

#include <cloudstorage/ICloudProvider.h>
#include <cloudstorage/IHttp.h>
#include <json/json.h>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>

namespace util {

void enqueue(std::function<void()> f);

}  // namespace util

#endif  // HTTP_UTILITY_H
