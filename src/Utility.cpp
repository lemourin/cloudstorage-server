#include "Utility.h"

Json::Value to_json(cloudstorage::ICloudProvider::Hints h) {
  Json::Value result;
  for (auto it : h) result[it.first] = it.second;
  return result;
}
