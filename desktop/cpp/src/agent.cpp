#include "agent.hpp"

#include "log.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dustx {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxFile = 1 << 20;
constexpr size_t kMaxExecOut = 256 << 10;
constexpr int kMaxList = 500;
constexpr int kExecTimeoutMs = 30000;

std::string path_utf8(const fs::path& p) {
#ifdef _WIN32
  return wide_to_utf8(p.wstring());
#else
  return p.string();
#endif
}

fs::path path_from_utf8(const std::string& s) {
#ifdef _WIN32
  return fs::path(utf8_to_wide(s));
#else
  return fs::path(s);
#endif
}

#ifdef _WIN32
std::ifstream open_in(const std::string& path) { return std::ifstream(utf8_to_wide(path), std::ios::binary); }
std::ofstream open_out(const std::string& path) { return std::ofstream(utf8_to_wide(path), std::ios::binary | std::ios::trunc); }
#else
std::ifstream open_in(const std::string& path) { return std::ifstream(path, std::ios::binary); }
std::ofstream open_out(const std::string& path) { return std::ofstream(path, std::ios::binary | std::ios::trunc); }
#endif

std::string agent_root() {
  const std::string override = getenv_or("DUSTX_AGENT_ROOT", "");
  if (!override.empty()) return override;
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return wide_to_utf8(buf);
#else
  const char* home = std::getenv("HOME");
  if (home && *home) return home;
#endif
  return {};
}

bool under_root(const fs::path& root, const fs::path& candidate) {
  const fs::path r = root.lexically_normal();
  const fs::path c = candidate.lexically_normal();
  const fs::path rel = c.lexically_relative(r);
  const std::string s = rel.generic_string();
  if (s.empty() || s == ".") return true;
  return s != ".." && s.rfind("../", 0) != 0;
}

bool resolve_path(const std::string& raw, fs::path* out, std::string* err) {
  const std::string root_s = agent_root();
  if (root_s.empty()) {
    *err = "no home directory";
    return false;
  }
  const fs::path root = path_from_utf8(root_s).lexically_normal();
  fs::path candidate;
  if (raw.empty() || raw == "." || raw == "/" || raw == "\\") {
    candidate = root;
  } else {
    const fs::path given = path_from_utf8(raw);
    candidate = given.is_absolute() ? given : (root / given);
    candidate = candidate.lexically_normal();
  }
  std::error_code ec;
  if (fs::exists(candidate, ec)) {
    candidate = fs::weakly_canonical(candidate, ec);
    if (ec) {
      *err = "invalid path";
      return false;
    }
  }
  if (!under_root(root, candidate)) {
    *err = "path outside home";
    return false;
  }
  *out = candidate;
  return true;
}

std::string err_json(const std::string& msg, const std::string& path = {}) {
  std::ostringstream o;
  o << "{\"ok\":false,\"error\":\"" << json_escape(msg) << "\"";
  if (!path.empty()) o << ",\"path\":\"" << json_escape(path) << "\"";
  o << "}";
  return o.str();
}

std::string list_dir(const fs::path& target) {
  std::error_code ec;
  if (!fs::exists(target, ec)) return err_json("not found", path_utf8(target));
  if (!fs::is_directory(target, ec)) return err_json("not a directory", path_utf8(target));
  std::vector<fs::path> items;
  for (const auto& entry : fs::directory_iterator(target, fs::directory_options::skip_permission_denied, ec)) {
    items.push_back(entry.path());
  }
  std::sort(items.begin(), items.end(), [](const fs::path& a, const fs::path& b) {
    return path_utf8(a.filename()) < path_utf8(b.filename());
  });
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"list\",\"path\":\"" << json_escape(path_utf8(target)) << "\",\"entries\":[";
  int n = 0;
  for (const auto& item : items) {
    if (n >= kMaxList) break;
    std::error_code st_ec;
    const bool is_dir = fs::is_directory(item, st_ec);
    uintmax_t size = 0;
    if (!is_dir) size = fs::file_size(item, st_ec);
    if (n) o << ",";
    o << "{\"name\":\"" << json_escape(path_utf8(item.filename())) << "\",\"dir\":" << (is_dir ? "true" : "false")
      << ",\"size\":" << static_cast<unsigned long long>(is_dir ? 0 : size) << "}";
    ++n;
  }
  o << "]}";
  return o.str();
}

std::string read_file_op(const fs::path& target) {
  std::error_code ec;
  if (!fs::is_regular_file(target, ec)) return err_json("not a file", path_utf8(target));
  const auto size = fs::file_size(target, ec);
  if (ec || size > kMaxFile) {
    return err_json(ec ? "read failed" : "file too large", path_utf8(target));
  }
  auto in = open_in(path_utf8(target));
  if (!in) return err_json("read failed", path_utf8(target));
  std::string data(static_cast<size_t>(size), '\0');
  in.read(data.data(), static_cast<std::streamsize>(size));
  if (!in && !in.eof()) return err_json("read failed", path_utf8(target));
  const auto got = in.gcount();
  data.resize(got > 0 ? static_cast<size_t>(got) : 0);
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"read\",\"path\":\"" << json_escape(path_utf8(target)) << "\",\"content\":\""
    << json_escape(data) << "\"}";
  return o.str();
}

std::string write_file_op(const fs::path& target, const std::string& content) {
  if (content.size() > kMaxFile) return err_json("content too large");
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  auto out = open_out(path_utf8(target));
  if (!out) return err_json("write failed", path_utf8(target));
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) return err_json("write failed", path_utf8(target));
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"write\",\"path\":\"" << json_escape(path_utf8(target)) << "\",\"bytes\":"
    << content.size() << "}";
  return o.str();
}

#ifdef _WIN32
std::wstring system_cmd() {
  wchar_t dir[MAX_PATH];
  const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return L"cmd.exe";
  return std::wstring(dir) + L"\\cmd.exe";
}

std::string run_exec(const std::string& command, const fs::path& cwd) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return err_json("exec failed");
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  const std::wstring exe = system_cmd();
  std::wstring line = L"\"" + exe + L"\" /c " + utf8_to_wide(command);
  std::vector<wchar_t> buf(line.begin(), line.end());
  buf.push_back(0);
  const std::wstring cwd_w = cwd.wstring();
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 cwd_w.c_str(), &si, &pi);
  CloseHandle(write_pipe);
  if (!ok) {
    CloseHandle(read_pipe);
    return err_json("exec failed");
  }

  std::string acc;
  char chunk[4096];
  DWORD got = 0;
  const DWORD deadline = GetTickCount() + kExecTimeoutMs;
  bool timed_out = false;
  for (;;) {
    const DWORD left = deadline - GetTickCount();
    if (static_cast<int>(left) <= 0) {
      timed_out = true;
      break;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
    while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &got, nullptr) && got) {
      DWORD n = 0;
      if (!ReadFile(read_pipe, chunk, sizeof(chunk), &n, nullptr) || !n) break;
      if (acc.size() < kMaxExecOut) {
        const size_t room = kMaxExecOut - acc.size();
        acc.append(chunk, n < room ? n : room);
      }
    }
    if (wait == WAIT_OBJECT_0) break;
  }
  if (timed_out) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 2000);
  }
  while (ReadFile(read_pipe, chunk, sizeof(chunk), &got, nullptr) && got) {
    if (acc.size() < kMaxExecOut) {
      const size_t room = kMaxExecOut - acc.size();
      acc.append(chunk, got < room ? got : room);
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(read_pipe);
  if (timed_out) return err_json("timeout");
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"exec\",\"exit\":" << static_cast<int>(code) << ",\"stdout\":\"" << json_escape(acc)
    << "\",\"stderr\":\"\"}";
  return o.str();
}
#else
void drain_fd(int fd, std::string* acc) {
  char buf[4096];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    if (acc->size() < kMaxExecOut) {
      const size_t room = kMaxExecOut - acc->size();
      acc->append(buf, static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room);
    }
  }
}

std::string run_exec(const std::string& command, const fs::path& cwd) {
  int outp[2];
  int errp[2];
  if (pipe(outp) != 0 || pipe(errp) != 0) return err_json("exec failed");
  const pid_t pid = fork();
  if (pid < 0) {
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    return err_json("exec failed");
  }
  if (pid == 0) {
    if (!cwd.empty()) {
      if (chdir(path_utf8(cwd).c_str()) != 0) _exit(127);
    }
    dup2(outp[1], STDOUT_FILENO);
    dup2(errp[1], STDERR_FILENO);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  close(outp[1]);
  close(errp[1]);
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  std::string out;
  std::string err;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kExecTimeoutMs);
  int status = 0;
  bool timed_out = false;
  bool exited = false;
  while (!exited) {
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      break;
    }
    pollfd fds[2]{{outp[0], POLLIN, 0}, {errp[0], POLLIN, 0}};
    poll(fds, 2, 100);
    if (fds[0].revents & POLLIN) drain_fd(outp[0], &out);
    if (fds[1].revents & POLLIN) drain_fd(errp[0], &err);
    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      exited = true;
      drain_fd(outp[0], &out);
      drain_fd(errp[0], &err);
    }
  }
  close(outp[0]);
  close(errp[0]);
  if (timed_out) return err_json("timeout");
  int code = 1;
  if (WIFEXITED(status)) code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"exec\",\"exit\":" << code << ",\"stdout\":\"" << json_escape(out) << "\",\"stderr\":\""
    << json_escape(err) << "\"}";
  return o.str();
}
#endif

}  // namespace

std::string agent_run(const std::string& body) {
  const std::string op = json_get_string(body, "op");
  const std::string path = json_get_string(body, "path");
  const std::string content = json_get_string(body, "content");
  const std::string command = json_get_string(body, "command");
  const std::string cwd = json_get_string(body, "cwd");
  log_info("agent", op + (path.empty() ? "" : " " + path));
  if (op == "exec") {
    if (command.empty()) return err_json("command required");
    fs::path work;
    std::string err;
    if (!resolve_path(cwd, &work, &err)) return err_json(err);
    std::error_code ec;
    if (!fs::is_directory(work, ec)) return err_json("cwd is not a directory", path_utf8(work));
    return run_exec(command, work);
  }
  if (op != "list" && op != "read" && op != "write") {
    return err_json("unknown op (list/read/write/exec)");
  }
  fs::path target;
  std::string err;
  if (!resolve_path(path, &target, &err)) return err_json(err);
  if (op == "list") return list_dir(target);
  if (op == "read") return read_file_op(target);
  return write_file_op(target, content);
}

}  // namespace dustx
