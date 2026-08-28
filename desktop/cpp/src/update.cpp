#include "update.hpp"

#include "device_info.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "util.hpp"
#include "version.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <mach-o/dyld.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dustx {
namespace fs = std::filesystem;
namespace {

struct UpdateInfo {
  bool ok = false;
  bool newer = false;
  bool force = false;
  std::string current;
  std::string latest;
  std::string notes;
  std::string url;
  std::string filename;
  std::string sha256;
  std::string error;
  long long size = 0;
};

std::mutex g_mu;
UpdateInfo g_last;
std::string g_phase = "idle";
std::string g_phase_err;
std::function<void()> g_quit;
std::atomic<bool> g_applying{false};
std::atomic<bool> g_watch_started{false};

#ifdef _WIN32
fs::path path_from_utf8(const std::string& s) { return fs::path(utf8_to_wide(s)); }
std::string path_to_utf8(const fs::path& p) { return wide_to_utf8(p.wstring()); }
#else
fs::path path_from_utf8(const std::string& s) { return fs::path(s); }
std::string path_to_utf8(const fs::path& p) { return p.string(); }
#endif

std::string slash_path(std::string p) {
  for (char& c : p) {
    if (c == '\\') c = '/';
  }
  return p;
}

bool write_text(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

int run_hidden(const std::string& cmd, int timeout_ms) {
#ifdef _WIN32
  std::wstring wcmd = utf8_to_wide(std::string("cmd.exe /c ") + cmd);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE);
  DWORD code = 1;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else {
    TerminateProcess(pi.hProcess, 1);
    code = 1;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
#else
  (void)timeout_ms;
  const int st = std::system(cmd.c_str());
  if (st == -1) return -1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  return 1;
#endif
}

bool curl_to_file(const std::string& url, const fs::path& dest, int timeout_sec, std::string* err) {
  if (url.empty()) {
    if (err) *err = "没有下载地址";
    return false;
  }
  const fs::path cfg = dest.string() + std::string(".curl.cfg");
  std::ostringstream o;
  o << "url = \"" << url << "\"\n"
    << "output = \"" << slash_path(path_to_utf8(dest)) << "\"\n"
    << "silent\nshow-error\nlocation\nfail\nretry = 3\n"
    << "max-time = " << timeout_sec << "\n";
  if (!use_system_proxy()) o << "noproxy = \"*\"\n";
  if (!write_text(cfg, o.str())) {
    if (err) *err = "无法写下载配置";
    return false;
  }
#ifdef _WIN32
  const char* curl_bin = "C:/Windows/System32/curl.exe";
#else
  const char* curl_bin = "curl";
#endif
  const std::string cmd = std::string(curl_bin) + " --config \"" + slash_path(path_to_utf8(cfg)) + "\"";
  const int code = run_hidden(cmd, (timeout_sec + 15) * 1000);
  std::error_code ec;
  fs::remove(cfg, ec);
  if (code != 0) {
    if (err) *err = "下载失败";
    return false;
  }
  return true;
}

std::string capture_cmd(const std::string& cmd) {
#ifdef _WIN32
  std::wstring wcmd = utf8_to_wide(std::string("cmd.exe /c ") + cmd);
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  if (!CreatePipe(&read, &write, &sa, 0)) return {};
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = write;
  si.hStdError = write;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(read);
    CloseHandle(write);
    return {};
  }
  CloseHandle(write);
  std::string out;
  char buf[512];
  DWORD n = 0;
  while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, buf + n);
  WaitForSingleObject(pi.hProcess, 15000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(read);
  return out;
#else
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {};
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), pipe)) out += buf;
  pclose(pipe);
  return out;
#endif
}

std::string file_sha256_hex(const fs::path& path) {
#ifdef _WIN32
  const std::string out = capture_cmd(std::string("certutil -hashfile \"") + path_to_utf8(path) + "\" SHA256");
#else
  const std::string out = capture_cmd(std::string("shasum -a 256 \"") + path_to_utf8(path) + "\"");
#endif
  // certutil prints "SHA256 hash of file:" first; do not treat the "256" as the digest.
  // Hash bytes may be spaced ("4c ca 0c ..."). Ignore spaces; reset on other junk.
  std::string hex;
  for (char c : out) {
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if ((l >= '0' && l <= '9') || (l >= 'a' && l <= 'f')) {
      hex.push_back(l);
      if (hex.size() == 64) return hex;
    } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      hex.clear();
    }
  }
  return {};
}

bool version_less(const std::string& a, const std::string& b) {
  std::vector<int> left, right;
  auto parse = [](const std::string& s, std::vector<int>& out) {
    int n = 0;
    bool in = false;
    for (char c : s) {
      if (c >= '0' && c <= '9') {
        n = n * 10 + (c - '0');
        in = true;
      } else if (in) {
        out.push_back(n);
        n = 0;
        in = false;
      }
    }
    if (in) out.push_back(n);
    if (out.empty()) out.push_back(0);
  };
  parse(a, left);
  parse(b, right);
  const size_t n = left.size() > right.size() ? left.size() : right.size();
  left.resize(n, 0);
  right.resize(n, 0);
  return left < right;
}

fs::path cache_dir() {
#ifdef _WIN32
  const std::string base = getenv_or("LOCALAPPDATA", "");
  fs::path dir = path_from_utf8(base.empty() ? "." : base) / "DustX" / "update";
#else
  const std::string home = getenv_or("HOME", "");
  fs::path dir = path_from_utf8(home.empty() ? "/tmp" : home) / "Library" / "Caches" / "DustX" / "update";
#endif
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

#ifdef _WIN32
std::string exe_path() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return n ? wide_to_utf8(std::wstring(buf, buf + n)) : std::string();
}
#else
std::string exe_path() {
  char buf[4096];
  uint32_t n = sizeof(buf);
  if (_NSGetExecutablePath(buf, &n) != 0) return {};
  char real[4096];
  if (realpath(buf, real)) return real;
  return buf;
}

std::string bundle_path() {
  fs::path exe = path_from_utf8(exe_path());
  if (exe.empty()) return {};
  if (exe.parent_path().filename() == "MacOS" && exe.parent_path().parent_path().filename() == "Contents") {
    return path_to_utf8(exe.parent_path().parent_path().parent_path());
  }
  return {};
}
#endif

std::string find_payload(const fs::path& root) {
  std::error_code ec;
  if (!fs::exists(root, ec)) return {};
#ifdef _WIN32
  if (fs::is_regular_file(root / "DustX.exe", ec)) return path_to_utf8(root);
#endif
  for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
#ifdef _WIN32
    if (it->path().filename() == L"DustX.exe") return path_to_utf8(it->path().parent_path());
#else
    if (it->path().extension() == ".app") return path_to_utf8(it->path());
#endif
  }
  return {};
}

#ifdef _WIN32
bool spawn_helper(const fs::path& script, const std::vector<std::string>& args) {
  std::ostringstream o;
  o << "cmd.exe /c \"\"" << path_to_utf8(script) << "\"";
  for (const auto& a : args) o << " \"" << a << "\"";
  o << "\"";
  std::wstring wcmd = utf8_to_wide(o.str());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#else
bool spawn_helper(const fs::path& script, const std::vector<std::string>& args) {
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    const std::string sh = path_to_utf8(script);
    const char* a0 = args.size() > 0 ? args[0].c_str() : "";
    const char* a1 = args.size() > 1 ? args[1].c_str() : "";
    const char* a2 = args.size() > 2 ? args[2].c_str() : "";
    execl("/bin/bash", "bash", sh.c_str(), a0, a1, a2, static_cast<char*>(nullptr));
    _exit(127);
  }
  return true;
}
#endif

void set_phase(const std::string& phase, const std::string& err = {}) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_phase = phase;
  g_phase_err = err;
}

void normalize_info(UpdateInfo& info) {
  info.current = app_version();
  if (info.latest.empty() || !version_less(info.current, info.latest)) {
    info.newer = false;
    info.force = false;
    info.notes.clear();
  }
}

UpdateInfo parse_remote(const std::string& json) {
  UpdateInfo info;
  info.ok = json_get_bool(json, "ok", false);
  info.newer = json_get_bool(json, "newer", false);
  info.force = json_get_bool(json, "force", false);
  info.latest = json_get_string(json, "latest");
  info.notes = json_get_string(json, "notes");
  info.url = json_get_string(json, "url");
  info.filename = json_get_string(json, "filename");
  info.sha256 = json_get_string(json, "sha256");
  info.size = json_get_ll(json, "size", 0);
  info.error = json_get_string(json, "error");
  if (info.error.empty()) info.error = json_get_string(json, "message");
  if (!info.ok && info.error.empty()) info.error = "无法获取更新信息";
  normalize_info(info);
  return info;
}

UpdateInfo fetch_remote() {
  UpdateInfo info;
  info.current = app_version();
#ifdef _WIN32
  const char* plat = "windows";
#else
  const char* plat = "macos";
#endif
  const fs::path dest = cache_dir() / "info.json";
  std::error_code ec;
  fs::remove(dest, ec);
  const std::string origin = signaling_http_origin();
  const std::string url = origin + "/api/update?platform=" + plat + "&current=" + info.current;
  std::string err;
  if (curl_to_file(url, dest, 20, &err)) {
    info = parse_remote(read_file(path_to_utf8(dest)));
    if (info.ok) return info;
  }
  const std::string fallback = origin + "/api/downloads/" + plat;
  fs::remove(dest, ec);
  if (!curl_to_file(fallback, dest, 20, &err)) {
    info.error = err.empty() ? "无法获取更新信息" : err;
    normalize_info(info);
    return info;
  }
  const std::string json = read_file(path_to_utf8(dest));
  info = parse_remote(json);
  info.ok = json_get_bool(json, "ok", false);
  if (info.latest.empty()) info.latest = json_get_string(json, "version");
  info.newer = !info.latest.empty() && version_less(info.current, info.latest);
  info.force = false;
  if (!info.ok && info.error.empty()) info.error = "无法获取更新信息";
  normalize_info(info);
  return info;
}

std::string info_json(const UpdateInfo& info, const std::string& phase, const std::string& err) {
  std::ostringstream o;
  o << "{\"ok\":" << (info.ok ? "true" : "false")
    << ",\"current\":\"" << json_escape(info.current)
    << "\",\"latest\":\"" << json_escape(info.latest)
    << "\",\"notes\":\"" << json_escape(info.notes)
    << "\",\"newer\":" << (info.newer ? "true" : "false")
    << ",\"force\":" << (info.force ? "true" : "false")
    << ",\"size\":" << info.size
    << ",\"phase\":\"" << json_escape(phase)
    << "\",\"error\":\"" << json_escape(err.empty() ? info.error : err) << "\"}";
  return o.str();
}

bool extract_zip(const fs::path& zip, const fs::path& dest, std::string* err) {
  std::error_code ec;
  fs::remove_all(dest, ec);
  fs::create_directories(dest, ec);
#ifdef _WIN32
  const std::string cmd = std::string("tar -xf \"") + path_to_utf8(zip) + "\" -C \"" + path_to_utf8(dest) + "\"";
#else
  const std::string cmd = std::string("ditto -x -k \"") + path_to_utf8(zip) + "\" \"" + path_to_utf8(dest) + "\"";
#endif
  if (run_hidden(cmd, 120000) != 0) {
    if (err) *err = "解压失败";
    return false;
  }
  return true;
}

bool write_and_spawn_helper(const std::string& payload, std::string* err) {
#ifdef _WIN32
  const std::string exe = exe_path();
  if (exe.empty()) {
    if (err) *err = "找不到当前程序";
    return false;
  }
  const fs::path script = cache_dir() / "apply.cmd";
  const std::string body =
      "@echo off\r\n"
      "setlocal\r\n"
      "set \"EXE=%~1\"\r\n"
      "set \"SRC=%~2\"\r\n"
      "set \"PID=%~3\"\r\n"
      "set \"DIR=%~dp1\"\r\n"
      "set \"DIR=%DIR:~0,-1%\"\r\n"
      "powershell -NoProfile -WindowStyle Hidden -Command \"try { Wait-Process -Id %PID% -Timeout 90 -ErrorAction SilentlyContinue } catch {}\"\r\n"
      "taskkill /F /IM DustX.exe /T >nul 2>nul\r\n"
      "powershell -NoProfile -WindowStyle Hidden -Command \"Start-Sleep -Seconds 1\"\r\n"
      "if exist \"%DIR%\\DustX.exe.bak\" del /f /q \"%DIR%\\DustX.exe.bak\"\r\n"
      "if exist \"%DIR%\\DustX.exe\" ren \"%DIR%\\DustX.exe\" DustX.exe.bak\r\n"
      "copy /y \"%SRC%\\DustX.exe\" \"%DIR%\\DustX.exe\" >nul\r\n"
      "if not exist \"%DIR%\\DustX.exe\" goto fail\r\n"
      "fc /b \"%SRC%\\DustX.exe\" \"%DIR%\\DustX.exe\" >nul\r\n"
      "if errorlevel 1 goto fail\r\n"
      "if not exist \"%SRC%\\web\\shell.html\" goto fail\r\n"
      "robocopy \"%SRC%\" \"%DIR%\" /E /IS /IT /R:2 /W:1 /NFL /NDL /NJH /NJS /NC /NS /NP /XF DustX.exe\r\n"
      "if not exist \"%DIR%\\web\\shell.html\" goto fail\r\n"
      "if exist \"%DIR%\\DustX.exe.bak\" del /f /q \"%DIR%\\DustX.exe.bak\"\r\n"
      "start \"\" \"%DIR%\\DustX.exe\"\r\n"
      "exit /b 0\r\n"
      ":fail\r\n"
      "if exist \"%DIR%\\DustX.exe.bak\" (\r\n"
      "  del /f /q \"%DIR%\\DustX.exe\" >nul 2>nul\r\n"
      "  ren \"%DIR%\\DustX.exe.bak\" DustX.exe\r\n"
      ")\r\n"
      "if exist \"%DIR%\\DustX.exe\" start \"\" \"%DIR%\\DustX.exe\"\r\n"
      "exit /b 1\r\n";
  if (!write_text(script, body)) {
    if (err) *err = "无法写更新脚本";
    return false;
  }
  const std::string pid = std::to_string(GetCurrentProcessId());
  if (!spawn_helper(script, {exe, payload, pid})) {
    if (err) *err = "无法启动更新脚本";
    return false;
  }
#else
  const std::string app = bundle_path();
  if (app.empty()) {
    if (err) *err = "找不到当前应用包";
    return false;
  }
  const fs::path script = cache_dir() / "apply.sh";
  const std::string body =
      "#!/bin/bash\n"
      "APP=\"$1\"\n"
      "NEW=\"$2\"\n"
      "PID=\"$3\"\n"
      "while kill -0 \"$PID\" 2>/dev/null; do sleep 1; done\n"
      "rm -rf \"$APP\"\n"
      "ditto \"$NEW\" \"$APP\"\n"
      "open \"$APP\"\n";
  if (!write_text(script, body)) {
    if (err) *err = "无法写更新脚本";
    return false;
  }
  const std::string pid = std::to_string(getpid());
  if (!spawn_helper(script, {app, payload, pid})) {
    if (err) *err = "无法启动更新脚本";
    return false;
  }
#endif
  return true;
}

void request_quit() {
  log_warn("update", "即将退出以完成更新");
  std::function<void()> fn;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    fn = g_quit;
  }
  if (fn) fn();
#ifdef _WIN32
  else {
    log_error("update", "没有窗口回调，ExitProcess");
    ExitProcess(0);
  }
#else
  else _exit(0);
#endif
}

bool apply_worker(UpdateInfo info) {
  set_phase("downloading");
  log_info("update", std::string("开始下载 ") + info.latest);
  const fs::path zip = cache_dir() / (info.filename.empty() ? "dustx-update.zip" : info.filename);
  std::string err;
  if (!curl_to_file(info.url, zip, 600, &err)) {
    set_phase("error", err);
    g_applying = false;
    return false;
  }
  if (!info.sha256.empty()) {
    const std::string got = file_sha256_hex(zip);
    std::string want = info.sha256;
    for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (got != want) {
      set_phase("error", "校验失败");
      log_error("update", "更新包校验失败");
      g_applying = false;
      return false;
    }
  }
  set_phase("applying");
  const fs::path unpack = cache_dir() / "unpack";
  if (!extract_zip(zip, unpack, &err)) {
    set_phase("error", err);
    g_applying = false;
    return false;
  }
  const std::string payload = find_payload(unpack);
  if (payload.empty()) {
    set_phase("error", "更新包内容不对");
    g_applying = false;
    return false;
  }
#ifdef _WIN32
  if (!fs::exists(path_from_utf8(payload) / "web" / "shell.html")) {
    set_phase("error", "更新包缺少界面文件");
    g_applying = false;
    return false;
  }
#endif
  if (!write_and_spawn_helper(payload, &err)) {
    set_phase("error", err);
    g_applying = false;
    return false;
  }
  set_phase("restarting");
  log_info("update", "更新完成，即将重启");
  request_quit();
  return true;
}

}  // namespace

void set_update_quit(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_quit = std::move(fn);
}

void start_update_watcher() {
  bool expected = false;
  if (!g_watch_started.compare_exchange_strong(expected, true)) return;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    for (;;) {
      set_phase("checking");
      UpdateInfo info = fetch_remote();
      {
        std::lock_guard<std::mutex> lock(g_mu);
        g_last = info;
        if (g_phase == "checking") g_phase = info.ok ? "idle" : "error";
        if (!info.ok) g_phase_err = info.error;
      }
      log_info("update", std::string("检查 ok=") + (info.ok ? "1" : "0") + " force=" + (info.force ? "1" : "0") +
                             " newer=" + (info.newer ? "1" : "0") + " current=" + info.current +
                             " latest=" + info.latest);
      if (info.ok && info.force && info.newer && !g_applying.exchange(true)) {
        log_warn("update", std::string("强制更新 ") + info.current + " -> " + info.latest);
        if (apply_worker(info)) return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(45));
    }
  }).detach();
}

std::string check_update_json() {
  UpdateInfo info = fetch_remote();
  std::string phase;
  std::string err;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_last = info;
    phase = g_phase;
    err = g_phase_err;
  }
  return info_json(info, phase, err);
}

std::string update_status_json() {
  std::lock_guard<std::mutex> lock(g_mu);
  return info_json(g_last, g_phase, g_phase_err);
}

std::string apply_update_json(bool force) {
  UpdateInfo info = fetch_remote();
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_last = info;
    if (!info.newer) {
      g_phase = info.ok ? "idle" : "error";
      g_phase_err = info.ok ? std::string() : info.error;
    }
  }
  if (!info.ok) return info_json(info, "error", info.error);
  if (!info.newer) return info_json(info, "idle", {});
  if (!force && info.force) force = true;
  if (g_applying.exchange(true)) {
    return info_json(info, "applying", {});
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_last = info;
  }
  std::thread([info] { apply_worker(info); }).detach();
  return info_json(info, "downloading", {});
}

}  // namespace dustx
