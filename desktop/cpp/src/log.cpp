#include "log.hpp"

#include "util.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dustx {
namespace {

constexpr size_t kKeep = 400;
constexpr std::uintmax_t kRotateBytes = 2 * 1024 * 1024;

std::mutex g_mu;
std::deque<std::string> g_lines;
std::string g_path;
bool g_inited = false;

void mkdir_p(const std::string& dir) {
#ifdef _WIN32
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '\\' || dir[i] == '/' || i + 1 == dir.size()) {
      if (cur.size() > 2) _mkdir(cur.c_str());
    }
  }
#else
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '/' || i + 1 == dir.size()) mkdir(cur.c_str(), 0755);
  }
#endif
}

std::string default_path() {
#ifdef _WIN32
  wchar_t appdata[MAX_PATH];
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return "dustx.log";
  return wide_to_utf8(appdata) + "\\DustX\\dustx.log";
#else
  const char* home = std::getenv("HOME");
  if (!home || !*home) return "dustx.log";
  return std::string(home) + "/Library/Logs/DustX/dustx.log";
#endif
}

void ensure_init_locked() {
  if (g_inited) return;
  g_path = default_path();
  const auto slash = g_path.find_last_of("/\\");
  if (slash != std::string::npos) mkdir_p(g_path.substr(0, slash));
  g_inited = true;
}

std::string now_stamp() {
  using clock = std::chrono::system_clock;
  const auto t = clock::to_time_t(clock::now());
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count() % 1000;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
  return buf;
}

void rotate_if_needed_locked() {
  std::ifstream in(g_path, std::ios::binary | std::ios::ate);
  if (!in) return;
  const auto size = in.tellg();
  in.close();
  if (size < static_cast<std::streamoff>(kRotateBytes)) return;
  const std::string bak = g_path + ".1";
#ifdef _WIN32
  _unlink(bak.c_str());
  rename(g_path.c_str(), bak.c_str());
#else
  unlink(bak.c_str());
  rename(g_path.c_str(), bak.c_str());
#endif
}

void write_line(const char* level, const char* tag, const std::string& msg) {
  std::string line = now_stamp();
  line += " [";
  line += level;
  line += "] ";
  line += tag;
  line += ": ";
  line += msg;
  std::lock_guard<std::mutex> lock(g_mu);
  ensure_init_locked();
  g_lines.push_back(line);
  while (g_lines.size() > kKeep) g_lines.pop_front();
  rotate_if_needed_locked();
  std::ofstream out(g_path, std::ios::app | std::ios::binary);
  if (out) {
    out << line << '\n';
    out.flush();
  }
#ifdef _WIN32
  OutputDebugStringA((line + "\n").c_str());
#endif
}

}  // namespace

void log_info(const char* tag, const std::string& msg) { write_line("INFO", tag ? tag : "app", msg); }
void log_warn(const char* tag, const std::string& msg) { write_line("WARN", tag ? tag : "app", msg); }
void log_error(const char* tag, const std::string& msg) { write_line("ERROR", tag ? tag : "app", msg); }

std::string log_file_path() {
  std::lock_guard<std::mutex> lock(g_mu);
  ensure_init_locked();
  return g_path;
}

std::vector<std::string> log_recent(size_t n) {
  std::lock_guard<std::mutex> lock(g_mu);
  ensure_init_locked();
  if (n > g_lines.size()) n = g_lines.size();
  return std::vector<std::string>(g_lines.end() - static_cast<std::ptrdiff_t>(n), g_lines.end());
}

std::string logs_payload_json(size_t n) {
  const auto path = log_file_path();
  const auto lines = log_recent(n);
  std::ostringstream o;
  o << "{\"ok\":true,\"path\":\"" << json_escape(path) << "\",\"lines\":[";
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) o << ',';
    o << '"' << json_escape(lines[i]) << '"';
  }
  o << "]}";
  return o.str();
}

}  // namespace dustx
