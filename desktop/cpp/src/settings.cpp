#include "settings.hpp"

#include "recents.hpp"
#include "util.hpp"

#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace dustx {
namespace {

std::mutex g_mu;
std::function<void(bool)> g_apply;
bool g_loaded = false;
bool g_use_system = false;

std::string settings_path() {
#ifdef _WIN32
  return dustx_config_dir() + "\\settings.json";
#else
  return dustx_config_dir() + "/settings.json";
#endif
}

void ensure_dir() {
  const std::string dir = dustx_config_dir();
#ifdef _WIN32
  _mkdir(dir.c_str());
#else
  mkdir(dir.c_str(), 0755);
#endif
}

void load_locked() {
  if (g_loaded) return;
  g_loaded = true;
  g_use_system = false;
  const std::string raw = read_file(settings_path());
  if (raw.empty()) return;
  if (raw.find("\"use_system_proxy\"") == std::string::npos) return;
  g_use_system = json_get_bool(raw, "use_system_proxy", false);
}

}  // namespace

bool use_system_proxy() {
  std::lock_guard<std::mutex> lock(g_mu);
  load_locked();
  return g_use_system;
}

void set_use_system_proxy(bool on) {
  std::function<void(bool)> apply;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    if (g_use_system == on) return;
    g_use_system = on;
    ensure_dir();
    std::ofstream out(settings_path(), std::ios::trunc);
    if (out) out << "{\"use_system_proxy\":" << (on ? "true" : "false") << "}";
    apply = g_apply;
  }
  if (apply) apply(on);
}

std::string settings_json() {
  std::ostringstream o;
  o << "{\"ok\":true,\"use_system_proxy\":" << (use_system_proxy() ? "true" : "false") << "}";
  return o.str();
}

void apply_settings_body(const std::string& body) {
  if (body.find("use_system_proxy") != std::string::npos) {
    set_use_system_proxy(json_get_bool(body, "use_system_proxy", false));
  }
}

void set_proxy_apply(std::function<void(bool use_system)> fn) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_apply = std::move(fn);
}

}  // namespace dustx
