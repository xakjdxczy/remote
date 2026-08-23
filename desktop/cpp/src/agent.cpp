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
constexpr size_t kMaxChunk = 384 << 10;
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

bool is_abs_path(const std::string& raw) {
  if (raw.empty()) return false;
  return path_from_utf8(raw).is_absolute();
}

bool is_volume_root(const fs::path& p) {
  std::error_code ec;
  const fs::path n = fs::weakly_canonical(p, ec);
  const fs::path use = ec ? p.lexically_normal() : n;
  if (use == use.root_path()) return true;
  const std::string s = path_utf8(use);
  return s.size() == 3 && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
}

bool resolve_path(const std::string& raw, fs::path* out, std::string* err) {
  const std::string root_s = agent_root();
  if (root_s.empty()) {
    *err = "no home directory";
    return false;
  }
  const fs::path root = path_from_utf8(root_s).lexically_normal();
  fs::path candidate;
  if (raw.empty() || raw == ".") {
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
  if (!is_abs_path(raw) && !under_root(root, candidate)) {
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

std::string read_file_op(const fs::path& target, long long offset, long long length) {
  std::error_code ec;
  if (!fs::is_regular_file(target, ec)) return err_json("not a file", path_utf8(target));
  const auto size = fs::file_size(target, ec);
  if (ec) return err_json("read failed", path_utf8(target));
  if (offset > 0 || length > 0) {
    if (offset < 0 || static_cast<unsigned long long>(offset) > size) {
      return err_json("bad offset", path_utf8(target));
    }
    unsigned long long take = length > 0 ? static_cast<unsigned long long>(length)
                                         : std::min<unsigned long long>(kMaxChunk, size - offset);
    if (take > kMaxChunk) return err_json("chunk too large");
    auto in = open_in(path_utf8(target));
    if (!in) return err_json("read failed", path_utf8(target));
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string data(static_cast<size_t>(take), '\0');
    in.read(data.data(), static_cast<std::streamsize>(take));
    data.resize(in.gcount() > 0 ? static_cast<size_t>(in.gcount()) : 0);
    std::ostringstream o;
    o << "{\"ok\":true,\"op\":\"read\",\"path\":\"" << json_escape(path_utf8(target)) << "\",\"size\":" << size
      << ",\"offset\":" << offset << ",\"bytes\":" << data.size() << ",\"content_b64\":\""
      << json_escape(base64_encode(data)) << "\"}";
    return o.str();
  }
  if (size > kMaxFile) return err_json("file too large", path_utf8(target));
  auto in = open_in(path_utf8(target));
  if (!in) return err_json("read failed", path_utf8(target));
  std::string data(static_cast<size_t>(size), '\0');
  in.read(data.data(), static_cast<std::streamsize>(size));
  if (!in && !in.eof()) return err_json("read failed", path_utf8(target));
  const auto got = in.gcount();
  data.resize(got > 0 ? static_cast<size_t>(got) : 0);
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"read\",\"path\":\"" << json_escape(path_utf8(target)) << "\",\"size\":" << size
    << ",\"content\":\"" << json_escape(data) << "\"}";
  return o.str();
}

std::string write_file_op(const fs::path& target, const std::string& content, const std::string& content_b64,
                          long long offset) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (!content_b64.empty()) {
    const std::string data = base64_decode(content_b64);
    if (data.size() > kMaxChunk) return err_json("content too large");
    if (offset < 0) return err_json("bad offset");
    if (offset == 0) {
      auto out = open_out(path_utf8(target));
      if (!out) return err_json("write failed", path_utf8(target));
      out.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!out) return err_json("write failed", path_utf8(target));
    } else {
      if (!fs::exists(target, ec)) return err_json("file not found", path_utf8(target));
#ifdef _WIN32
      std::fstream out(utf8_to_wide(path_utf8(target)), std::ios::binary | std::ios::in | std::ios::out);
#else
      std::fstream out(path_utf8(target), std::ios::binary | std::ios::in | std::ios::out);
#endif
      if (!out) return err_json("write failed", path_utf8(target));
      out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
      out.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!out) return err_json("write failed", path_utf8(target));
    }
    std::ostringstream o;
    o << "{\"ok\":true,\"op\":\"write\",\"path\":\"" << json_escape(path_utf8(target)) << "\",\"bytes\":"
      << data.size() << ",\"offset\":" << offset << "}";
    return o.str();
  }
  if (content.size() > kMaxFile) return err_json("content too large");
  auto out = open_out(path_utf8(target));
  if (!out) return err_json("write failed", path_utf8(target));
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) return err_json("write failed", path_utf8(target));
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"write\",\"path\":\"" << json_escape(path_utf8(target)) << "\",\"bytes\":"
    << content.size() << "}";
  return o.str();
}

std::string mkdir_op(const fs::path& target) {
  std::error_code ec;
  fs::create_directories(target, ec);
  if (ec) return err_json(ec.message(), path_utf8(target));
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"mkdir\",\"path\":\"" << json_escape(path_utf8(target)) << "\"}";
  return o.str();
}

std::string rm_op(const fs::path& target) {
  std::error_code ec;
  if (!fs::exists(target, ec)) return err_json("not found", path_utf8(target));
  if (is_volume_root(target)) return err_json("refusing to delete a volume root", path_utf8(target));
  const auto n = fs::remove_all(target, ec);
  if (ec || n == 0) return err_json(ec ? ec.message() : "delete failed", path_utf8(target));
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"rm\",\"path\":\"" << json_escape(path_utf8(target)) << "\"}";
  return o.str();
}

std::string list_volumes() {
  std::ostringstream o;
  o << "{\"ok\":true,\"op\":\"volumes\",\"entries\":[";
  int n = 0;
#ifdef _WIN32
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(mask & (1u << i))) continue;
    const char letter = static_cast<char>('A' + i);
    std::string path(1, letter);
    path += ":\\";
    if (GetDriveTypeW(utf8_to_wide(path).c_str()) == DRIVE_NO_ROOT_DIR) continue;
    if (n) o << ",";
    o << "{\"name\":\"" << letter << ":\",\"path\":\"" << json_escape(path) << "\",\"dir\":true}";
    ++n;
  }
#else
  o << "{\"name\":\"/\",\"path\":\"/\",\"dir\":true}";
  n = 1;
  const char* extras[] = {"/Volumes", "/media", "/mnt"};
  for (const char* base : extras) {
    std::error_code ec;
    if (!fs::is_directory(base, ec)) continue;
    for (const auto& entry : fs::directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
      if (!entry.is_directory(ec)) continue;
      if (n) o << ",";
      o << "{\"name\":\"" << json_escape(path_utf8(entry.path().filename())) << "\",\"path\":\""
        << json_escape(path_utf8(entry.path())) << "\",\"dir\":true}";
      ++n;
    }
  }
#endif
  o << "]}";
  return o.str();
}

#ifdef _WIN32
std::wstring system_powershell() {
  wchar_t dir[MAX_PATH];
  const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    const std::wstring p = std::wstring(dir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
  }
  return L"powershell.exe";
}

bool looks_like_utf8(const std::string& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    if (p[i] < 0x80) {
      i += 1;
      continue;
    }
    int need = 0;
    if ((p[i] & 0xE0) == 0xC0) need = 1;
    else if ((p[i] & 0xF0) == 0xE0) need = 2;
    else if ((p[i] & 0xF8) == 0xF0) need = 3;
    else return false;
    if (i + static_cast<size_t>(need) >= n) return false;
    for (int j = 1; j <= need; ++j) {
      if ((p[i + j] & 0xC0) != 0x80) return false;
    }
    i += 1 + static_cast<size_t>(need);
  }
  return true;
}

std::string win_bytes_to_utf8(const std::string& raw) {
  if (raw.empty() || looks_like_utf8(raw)) return raw;
  const UINT cps[] = {GetConsoleOutputCP(), GetACP(), 936, 437};
  for (UINT cp : cps) {
    if (!cp) continue;
    const int wlen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (wlen <= 0) continue;
    std::wstring w(static_cast<size_t>(wlen), 0);
    if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), w.data(), wlen) <= 0) {
      continue;
    }
    return wide_to_utf8(w);
  }
  return raw;
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

  const std::wstring exe = system_powershell();
  const std::wstring script =
      L"[Console]::InputEncoding = New-Object System.Text.UTF8Encoding $false; "
      L"[Console]::OutputEncoding = [Console]::InputEncoding; "
      L"$OutputEncoding = [Console]::OutputEncoding; " +
      utf8_to_wide(command);
  const std::string encoded = base64_encode(
      std::string(reinterpret_cast<const char*>(script.data()), script.size() * sizeof(wchar_t)));
  const std::wstring line = L"\"" + exe +
                            L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " +
                            utf8_to_wide(encoded);
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
  acc = win_bytes_to_utf8(acc);
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
  const std::string content_b64 = json_get_string(body, "content_b64");
  const std::string command = json_get_string(body, "command");
  const std::string cwd = json_get_string(body, "cwd");
  const long long offset = json_get_ll(body, "offset", 0);
  const long long length = json_get_ll(body, "length", 0);
  log_info("agent", op + (path.empty() ? "" : " " + path));
  if (op == "volumes") return list_volumes();
  if (op == "exec") {
    if (command.empty()) return err_json("command required");
    fs::path work;
    std::string err;
    if (!resolve_path(cwd, &work, &err)) return err_json(err);
    std::error_code ec;
    if (!fs::is_directory(work, ec)) return err_json("cwd is not a directory", path_utf8(work));
    return run_exec(command, work);
  }
  if (op != "list" && op != "read" && op != "write" && op != "mkdir" && op != "rm") {
    return err_json("unknown op (list/read/write/exec/mkdir/rm/volumes)");
  }
  fs::path target;
  std::string err;
  if (!resolve_path(path, &target, &err)) return err_json(err);
  if (op == "list") return list_dir(target);
  if (op == "read") return read_file_op(target, offset, length);
  if (op == "write") return write_file_op(target, content, content_b64, offset);
  if (op == "mkdir") return mkdir_op(target);
  return rm_op(target);
}

}  // namespace dustx
