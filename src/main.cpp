#include <microhttpd.h>

#include <openssl/err.h>
#include <chrono>
#include <climits>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "HttpServer.h"
#include "Utility.h"

std::unique_ptr<std::mutex[]> crypto_locks;

struct crypto_lock {
  crypto_lock() {
    crypto_locks = std::make_unique<std::mutex[]>(CRYPTO_num_locks());
    CRYPTO_set_locking_callback([](int mode, int n, const char*, int) {
      if (mode & CRYPTO_LOCK)
        crypto_locks[n].lock();
      else
        crypto_locks[n].unlock();
    });
  }
};

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
  crypto_lock lock;
  return HttpServer(config).exec();
}
