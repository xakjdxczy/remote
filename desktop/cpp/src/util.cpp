#include "util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dustx {
namespace {

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  std::vector<uint8_t> buf(data, data + len);
  buf.push_back(0x80);
  while ((buf.size() % 64) != 56) buf.push_back(0);
  uint64_t bits = static_cast<uint64_t>(len) * 8;
  for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xff));

  for (size_t off = 0; off < buf.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(buf[off + i * 4]) << 24) | (uint32_t(buf[off + i * 4 + 1]) << 16) |
             (uint32_t(buf[off + i * 4 + 2]) << 8) | uint32_t(buf[off + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>((hs[i] >> 24) & 0xff);
    out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xff);
    out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xff);
    out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xff);
  }
}

std::string b64(const uint8_t* data, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = data[i] << 16;
    if (i + 1 < n) v |= data[i + 1] << 8;
    if (i + 2 < n) v |= data[i + 2];
    o.push_back(t[(v >> 18) & 63]);
    o.push_back(t[(v >> 12) & 63]);
    o.push_back(i + 1 < n ? t[(v >> 6) & 63] : '=');
    o.push_back(i + 2 < n ? t[v & 63] : '=');
  }
  return o;
}

}  // namespace

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o.push_back(static_cast<char>(c));
        }
    }
  }
  return o;
}

std::string json_get_string(const std::string& json, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  size_t pos = 0;
  while (true) {
    pos = json.find(pat, pos);
    if (pos == std::string::npos) return {};
    size_t i = pos + pat.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
    if (i >= json.size() || json[i] != ':') {
      pos += 1;
      continue;
    }
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
    if (i >= json.size() || json[i] != '"') return {};
    ++i;
    std::string out;
    while (i < json.size()) {
      char c = json[i++];
      if (c == '\\' && i < json.size()) {
        char e = json[i++];
        if (e == 'n') out.push_back('\n');
        else if (e == 'r') out.push_back('\r');
        else if (e == 't') out.push_back('\t');
        else out.push_back(e);
      } else if (c == '"') {
        return out;
      } else {
        out.push_back(c);
      }
    }
    return {};
  }
}

int json_get_int(const std::string& json, const std::string& key, int fallback) {
  const std::string pat = "\"" + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos) return fallback;
  size_t i = pos + pat.size();
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == ':')) ++i;
  if (i < json.size() && json[i] == '"') ++i;
  bool neg = false;
  if (i < json.size() && json[i] == '-') {
    neg = true;
    ++i;
  }
  if (i >= json.size() || json[i] < '0' || json[i] > '9') return fallback;
  int v = 0;
  while (i < json.size() && json[i] >= '0' && json[i] <= '9') v = v * 10 + (json[i++] - '0');
  return neg ? -v : v;
}

std::string signaling_http_origin() {
  std::string url = remote_console_url();
  auto scheme = url.find("://");
  if (scheme == std::string::npos) return "https://117.72.108.246";
  auto slash = url.find('/', scheme + 3);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string signaling_ws_url() {
  std::string origin = signaling_http_origin();
  if (origin.rfind("https://", 0) == 0) return "wss://" + origin.substr(8) + "/ws";
  if (origin.rfind("http://", 0) == 0) return "ws://" + origin.substr(7) + "/ws";
  return "wss://117.72.108.246/ws";
}

std::string ice_servers_json() {
  std::ostringstream o;
  o << "[{\"urls\":["
       "\"stun:stun.l.google.com:19302\","
       "\"stun:stun.qq.com:3478\","
       "\"stun:stun.miwifi.com:3478\","
       "\"stun:stun.cloudflare.com:3478\""
       "]}";
  const std::string urls = getenv_or("TURN_URLS", "");
  const std::string user = getenv_or("TURN_USER", "");
  const std::string pass = getenv_or("TURN_PASS", "");
  if (!urls.empty() && !user.empty() && !pass.empty()) {
    o << ",{\"urls\":[";
    bool first = true;
    size_t start = 0;
    while (start < urls.size()) {
      const auto comma = urls.find(',', start);
      std::string u = urls.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      while (!u.empty() && (u.front() == ' ' || u.front() == '\t')) u.erase(u.begin());
      while (!u.empty() && (u.back() == ' ' || u.back() == '\t')) u.pop_back();
      if (!u.empty()) {
        if (!first) o << ',';
        first = false;
        o << '"' << json_escape(u) << '"';
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    o << "],\"username\":\"" << json_escape(user) << "\",\"credential\":\"" << json_escape(pass) << "\"}";
  }
  o << "]";
  return o.str();
}

std::string make_token() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 999999);
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06d", dist(gen));
  return buf;
}

std::string sha1_base64(const std::string& data) {
  uint8_t digest[20];
  sha1(reinterpret_cast<const uint8_t*>(data.data()), data.size(), digest);
  return b64(digest, 20);
}

std::string mime_for(const std::string& path) {
  auto dot = path.rfind('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot);
  if (ext == ".html") return "text/html; charset=utf-8";
  if (ext == ".js") return "application/javascript; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".json") return "application/json; charset=utf-8";
  return "application/octet-stream";
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_all(int fd, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  size_t sent = 0;
  while (sent < n) {
#ifdef _WIN32
    int k = ::send(static_cast<SOCKET>(fd), p + sent, static_cast<int>(n - sent), 0);
#else
    ssize_t k = ::send(fd, p + sent, n - sent, 0);
#endif
    if (k <= 0) return false;
    sent += static_cast<size_t>(k);
  }
  return true;
}

bool read_some(int fd, void* data, size_t n, size_t& got) {
#ifdef _WIN32
  int k = ::recv(static_cast<SOCKET>(fd), static_cast<char*>(data), static_cast<int>(n), 0);
#else
  ssize_t k = ::recv(fd, data, n, 0);
#endif
  if (k <= 0) {
    got = 0;
    return false;
  }
  got = static_cast<size_t>(k);
  return true;
}

std::string getenv_or(const char* key, const char* fallback) {
  const char* v = std::getenv(key);
  if (v && *v) return v;
  return fallback;
}

std::string remote_console_url() {
  return getenv_or("DUSTX_REMOTE_URL", "https://117.72.108.246/remote/");
}

int default_listen_port() {
  const char* v = std::getenv("DUSTX_CAM_PORT");
  if (v && *v) {
    int p = std::atoi(v);
    if (p > 0 && p < 65536) return p;
  }
  return 18790;
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string wide_to_utf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string o(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), o.data(), n, nullptr, nullptr);
  return o;
}
#endif

}  // namespace dustx
