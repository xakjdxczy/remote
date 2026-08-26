#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dustx {

std::string json_escape(const std::string& s);
std::string json_get_string(const std::string& json, const std::string& key);
int json_get_int(const std::string& json, const std::string& key, int fallback);
bool json_get_bool(const std::string& json, const std::string& key, bool fallback);
long long json_get_ll(const std::string& json, const std::string& key, long long fallback);
std::string base64_encode(const std::string& data);
std::string base64_decode(const std::string& data);
std::string signaling_ws_url();
std::string signaling_http_origin();
std::string ice_servers_json();
std::string make_token();
std::string sha1_base64(const std::string& data);
std::string mime_for(const std::string& path);
std::string read_file(const std::string& path);
bool write_all(int fd, const void* data, size_t n);
bool read_some(int fd, void* data, size_t n, size_t& got);
std::string getenv_or(const char* key, const char* fallback);
std::string remote_console_url();
int default_listen_port();
void alert_error(const std::string& text);

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);
#endif

}  // namespace dustx
