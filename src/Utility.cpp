#include "Utility.h"
#include <fstream>
#include <sstream>

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
