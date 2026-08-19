#include "net.hpp"
#include "util.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdio>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace dustx {

void net_init() {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

void net_shutdown() {
#ifdef _WIN32
  WSACleanup();
#endif
}

void close_fd(int fd) {
  if (fd < 0) return;
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(fd));
#else
  close(fd);
#endif
}

void set_tcp_nodelay(int fd) {
  if (fd < 0) return;
#ifdef _WIN32
  BOOL yes = TRUE;
  setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
  int yes = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#endif
}

namespace {

bool read_exact_fd(int fd, void* data, size_t n) {
  auto* p = static_cast<char*>(data);
  size_t got_total = 0;
  while (got_total < n) {
    size_t got = 0;
    if (!read_some(fd, p + got_total, n - got_total, got)) return false;
    got_total += got;
  }
  return true;
}

}  // namespace

bool ws_write_frame(int fd, int opcode, const void* data, size_t n, std::mutex& mu) {
  uint8_t hdr[10];
  size_t hlen = 2;
  hdr[0] = static_cast<uint8_t>(0x80 | (opcode & 0x0f));
  if (n < 126) {
    hdr[1] = static_cast<uint8_t>(n);
  } else if (n < 65536) {
    hdr[1] = 126;
    hdr[2] = static_cast<uint8_t>((n >> 8) & 0xff);
    hdr[3] = static_cast<uint8_t>(n & 0xff);
    hlen = 4;
  } else {
    hdr[1] = 127;
    uint64_t v = n;
    for (int i = 0; i < 8; ++i) hdr[9 - i] = static_cast<uint8_t>((v >> (i * 8)) & 0xff);
    hlen = 10;
  }
  std::lock_guard<std::mutex> lock(mu);
  return write_all(fd, hdr, hlen) && (n == 0 || write_all(fd, data, n));
}

bool ws_read_frame(int fd, int& opcode, std::string& payload) {
  uint8_t hdr[2];
  if (!read_exact_fd(fd, hdr, 2)) return false;
  opcode = hdr[0] & 0x0f;
  bool masked = (hdr[1] & 0x80) != 0;
  uint64_t len = hdr[1] & 0x7f;
  if (len == 126) {
    uint8_t ext[2];
    if (!read_exact_fd(fd, ext, 2)) return false;
    len = (uint64_t(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8];
    if (!read_exact_fd(fd, ext, 8)) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }
  if (len > 8ull * 1024 * 1024) return false;
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked && !read_exact_fd(fd, mask, 4)) return false;
  payload.assign(static_cast<size_t>(len), '\0');
  if (len && !read_exact_fd(fd, payload.data(), static_cast<size_t>(len))) return false;
  if (masked) {
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
  }
  return true;
}

int listen_tcp(int port) {
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return -1;
  BOOL yes = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(static_cast<int>(s));
    return -1;
  }
  if (listen(s, 32) != 0) {
    close_fd(static_cast<int>(s));
    return -1;
  }
  return static_cast<int>(s);
}

int listen_tcp_loopback(int port) {
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return -1;
  BOOL yes = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(static_cast<int>(s));
    return -1;
  }
  if (listen(s, 32) != 0) {
    close_fd(static_cast<int>(s));
    return -1;
  }
  return static_cast<int>(s);
}

int connect_loopback(int port) {
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return -1;
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_fd(static_cast<int>(s));
    return -1;
  }
  return static_cast<int>(s);
}

int accept_fd(int listen_fd) {
#ifdef _WIN32
  SOCKET c = accept(static_cast<SOCKET>(listen_fd), nullptr, nullptr);
  if (c == INVALID_SOCKET) return -1;
  return static_cast<int>(c);
#else
  return accept(listen_fd, nullptr, nullptr);
#endif
}

std::vector<std::string> lan_ipv4s() {
  std::vector<std::string> out;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<char> buf(size);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                           nullptr, addrs, &size) == NO_ERROR) {
    for (auto* a = addrs; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp) continue;
      for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
        char ip[64];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        if (std::strncmp(ip, "127.", 4) != 0) out.emplace_back(ip);
      }
    }
  }
#else
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) == 0) {
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
      char ip[64];
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, ip, sizeof(ip));
      if (std::strncmp(ip, "127.", 4) != 0) out.emplace_back(ip);
    }
    freeifaddrs(ifa);
  }
#endif
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static std::string exe_dir() {
#ifdef _WIN32
  wchar_t wpath[MAX_PATH];
  GetModuleFileNameW(nullptr, wpath, MAX_PATH);
  std::wstring w(wpath);
  auto slash = w.find_last_of(L"\\/");
  if (slash != std::wstring::npos) w.resize(slash);
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string o(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), o.data(), n, nullptr, nullptr);
  return o;
#else
  char path[1024];
#ifdef __APPLE__
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) != 0) return ".";
#else
  ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0) return ".";
  path[n] = 0;
#endif
  char resolved[1024];
  if (!realpath(path, resolved)) {
    std::strncpy(resolved, path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = 0;
  }
  std::string full(resolved);
  auto slash = full.find_last_of('/');
  if (slash != std::string::npos) full.resize(slash);
  return full;
#endif
}

static bool can_exec(const std::string& p) {
#ifdef _WIN32
  return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  return access(p.c_str(), X_OK) == 0;
#endif
}

std::string find_adb() {
  const std::string dir = exe_dir();
  std::vector<std::string> cands;
#ifdef _WIN32
  cands.push_back(dir + "\\platform-tools\\adb.exe");
  if (const char* ah = std::getenv("ANDROID_HOME")) cands.push_back(std::string(ah) + "\\platform-tools\\adb.exe");
  if (const char* home = std::getenv("LOCALAPPDATA"))
    cands.push_back(std::string(home) + "\\Android\\Sdk\\platform-tools\\adb.exe");
#else
  cands.push_back(dir + "/../Resources/platform-tools/adb");
  cands.push_back(dir + "/platform-tools/adb");
  cands.push_back("/opt/homebrew/bin/adb");
  cands.push_back("/usr/local/bin/adb");
  if (const char* ah = std::getenv("ANDROID_HOME")) cands.push_back(std::string(ah) + "/platform-tools/adb");
  if (const char* home = std::getenv("HOME")) {
    cands.push_back(std::string(home) + "/Library/Android/sdk/platform-tools/adb");
    cands.push_back(std::string(home) + "/Android/Sdk/platform-tools/adb");
  }
#endif
  for (const auto& p : cands) {
    if (can_exec(p)) return p;
  }
  return {};
}

std::string run_adb(const std::string& args) {
  const std::string adb = find_adb();
  if (adb.empty()) return "adb: command not found";
#ifdef _WIN32
  const auto slash = adb.find_last_of("\\/");
  const std::string folder = slash == std::string::npos ? "." : adb.substr(0, slash);
  return run_command("set PATH=" + folder + ";%PATH%&& \"" + adb + "\" " + args + " 2>&1");
#else
  const auto slash = adb.rfind('/');
  const std::string folder = slash == std::string::npos ? "." : adb.substr(0, slash);
  return run_command("PATH=\"" + folder + ":$PATH\" \"" + adb + "\" " + args + " 2>&1");
#endif
}

static std::string trim_copy(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::vector<AdbDevice> list_adb_devices() {
  run_adb("start-server");
  const std::string text = run_adb("devices");
  std::vector<AdbDevice> out;
  std::string line;
  std::istringstream in(text);
  while (std::getline(in, line)) {
    line = trim_copy(line);
    if (line.empty() || line[0] == '*' || line.rfind("List of", 0) == 0) continue;
    const auto sp = line.find_last_of(" \t");
    if (sp == std::string::npos || sp == 0) continue;
    AdbDevice d;
    d.serial = trim_copy(line.substr(0, sp));
    d.state = trim_copy(line.substr(sp + 1));
    if (d.serial.empty() || d.state.empty()) continue;
    if (d.state == "device" || d.state == "unauthorized" || d.state == "offline" || d.state == "no permissions") {
      out.push_back(std::move(d));
    }
  }
  return out;
}

static std::vector<std::string> ioreg_usb_names() {
  std::vector<std::string> names;
#ifdef __APPLE__
  const std::string raw = run_command("ioreg -l -w0 -c IOUSBHostDevice 2>/dev/null");
  const std::string key = "\"USB Product Name\" = \"";
  size_t pos = 0;
  while ((pos = raw.find(key, pos)) != std::string::npos) {
    pos += key.size();
    const auto end = raw.find('"', pos);
    if (end == std::string::npos) break;
    std::string name = raw.substr(pos, end - pos);
    pos = end + 1;
    if (name.empty() || name.find("Apple") != std::string::npos || name.find("Hub") != std::string::npos) continue;
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
#endif
  return names;
}

std::string usb_phone_hint() {
  const auto names = ioreg_usb_names();
  if (names.empty()) return {};
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out += "、";
    out += names[i];
  }
  return out;
}

bool usb_has_adb_interface() {
#ifdef __APPLE__
  const std::string raw = run_command("ioreg -l -w0 2>/dev/null");
  return raw.find("ADB Interface") != std::string::npos || raw.find("Android Debug") != std::string::npos;
#else
  return false;
#endif
}

std::string run_command(const std::string& cmd) {
#ifdef _WIN32
  FILE* pipe = _popen(cmd.c_str(), "r");
#else
  FILE* pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe) return {};
  std::string text;
  std::array<char, 512> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) text += buf.data();
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return text;
}

}  // namespace dustx
