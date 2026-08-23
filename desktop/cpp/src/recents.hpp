#pragma once

#include <string>

namespace dustx {

std::string dustx_config_dir();
std::string load_connections_json();
bool upsert_connection(const std::string& body, std::string* err);
bool remove_connection(const std::string& body, std::string* err);

}  // namespace dustx
