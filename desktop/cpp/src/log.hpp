#pragma once

#include <string>
#include <vector>

namespace dustx {

void log_info(const char* tag, const std::string& msg);
void log_warn(const char* tag, const std::string& msg);
void log_error(const char* tag, const std::string& msg);
std::string log_file_path();
std::vector<std::string> log_recent(size_t n);
std::string logs_payload_json(size_t n = 250);

}  // namespace dustx
