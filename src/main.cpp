#include <microhttpd.h>

#include <chrono>
#include <climits>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "HttpServer.h"
#include "Utility.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " config_file"
              << "\n";
    return 1;
  }
  std::stringstream stream;
  std::fstream f(argv[1]);
  stream << f.rdbuf();
  Json::Value config;
  if (!Json::Reader().parse(stream.str(), config)) {
    std::cerr << "invalid config\n";
    return 1;
  }
  return HttpServer(config).exec();
}
