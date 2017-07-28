#include <microhttpd.h>

#include <chrono>
#include <climits>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "HttpServer.h"
#include "Utility.h"

using namespace std::string_literals;

int http_request_callback(void* cls, MHD_Connection* connection,
                          const char* url, const char* /*method*/,
                          const char* /*version*/, const char* /*upload_data*/,
                          size_t* /*upload_data_size*/, void** /*ptr*/) {
  std::cerr << "got request " << url << "\n";
  HttpServer* data = static_cast<HttpServer*>(cls);
  Json::Value result;
  const char* key =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "key");
  if (!key) {
    result["error"] = "missing key";
  } else {
    const char* provider = MHD_lookup_connection_value(
        connection, MHD_GET_ARGUMENT_KIND, "provider");
    if (provider) {
      auto p = data->provider(key + " "s + provider);
      if (url == "/exchange_code"s) {
        result = p->exchange_code(connection);
      } else if (url == "/list_directory"s) {
        result = p->list_directory(connection);
      } else if (url == "/get_item_data"s) {
        result = p->get_item_data(connection);
      }
    } else {
      if (url == "/list_providers"s) result = data->list_providers(connection);
    }
  }

  auto str = Json::StyledWriter().write(result);
  MHD_Response* response = MHD_create_response_from_buffer(
      str.length(), (void*)str.c_str(), MHD_RESPMEM_MUST_COPY);
  MHD_add_response_header(response, "Content-Type", "application/json");
  int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  MHD_destroy_response(response);
  return ret;
}

void run_server(Json::Value config) {
  HttpServer data(config);
  auto http_server =
      create_server(IHttpServer::Type::SingleThreaded, config["port"].asInt(),
                    http_request_callback, &data, "", "");
  std::this_thread::sleep_for(std::chrono::seconds(INT32_MAX));
}

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
  run_server(config);
}
