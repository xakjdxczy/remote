#include "server.hpp"

#include "net.hpp"
#include "ssh_host.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <stdlib.h>
#include <unistd.h>
#endif

namespace dustx {
namespace {

struct HttpReq {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

std::string header_get(const HttpReq& req, const std::string& key) {
  auto it = req.headers.find(key);
  return it == req.headers.end() ? std::string() : it->second;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string exe_dir() {
#ifdef _WIN32
  wchar_t wpath[MAX_PATH];
  GetModuleFileNameW(nullptr, wpath, MAX_PATH);
  std::wstring w(wpath);
  auto slash = w.find_last_of(L"\\/");
  if (slash != std::wstring::npos) w.resize(slash);
  return wide_to_utf8(w);
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
  if (!realpath(path, resolved)) std::strncpy(resolved, path, sizeof(resolved) - 1);
  std::string full(resolved);
  auto slash = full.find_last_of('/');
  if (slash != std::string::npos) full.resize(slash);
  return full;
#endif
}

std::string find_web_dir() {
  const char* env = std::getenv("DUSTX_WEB_DIR");
  if (env && *env) return env;
  std::string dir = exe_dir();
  const char* cands[] = {
      "/../Resources/web",
      "/web",
      "/../../web",
      "/../../../desktop/cpp/web",
  };
  for (const char* rel : cands) {
    std::string p = dir + rel;
    if (!read_file(p + "/cam.html").empty()) return p;
  }
  return dir + "/web";
}

bool read_http(int fd, HttpReq& req) {
  std::string raw;
  char buf[4096];
  size_t header_end = std::string::npos;
  while (raw.size() < 64 * 1024) {
    size_t got = 0;
    if (!read_some(fd, buf, sizeof(buf), got)) return false;
    raw.append(buf, got);
    header_end = raw.find("\r\n\r\n");
    if (header_end != std::string::npos) break;
  }
  if (header_end == std::string::npos) return false;
  std::istringstream in(raw.substr(0, header_end));
  std::string line;
  if (!std::getline(in, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream first(line);
  first >> req.method >> req.path;
  auto q = req.path.find('?');
  if (q != std::string::npos) req.path.resize(q);
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string k = lower(line.substr(0, colon));
    std::string v = line.substr(colon + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    req.headers[k] = v;
  }
  size_t already = raw.size() - (header_end + 4);
  req.body = raw.substr(header_end + 4);
  size_t want = 0;
  auto cl = header_get(req, "content-length");
  if (!cl.empty()) want = static_cast<size_t>(std::strtoul(cl.c_str(), nullptr, 10));
  if (want > 8 * 1024 * 1024) return false;
  while (req.body.size() < want) {
    size_t got = 0;
    if (!read_some(fd, buf, sizeof(buf), got)) return false;
    req.body.append(buf, got);
  }
  (void)already;
  return true;
}

void http_reply(int fd, int code, const char* reason, const std::string& mime, const std::string& body) {
  std::ostringstream o;
  o << "HTTP/1.1 " << code << ' ' << reason
    << "\r\nContent-Type: " << mime
    << "\r\nContent-Length: " << body.size()
    << "\r\nConnection: close"
    << "\r\nAccess-Control-Allow-Origin: *"
    << "\r\n\r\n"
    << body;
  const auto s = o.str();
  write_all(fd, s.data(), s.size());
}

bool ws_handshake(int fd, const HttpReq& req) {
  std::string key = header_get(req, "sec-websocket-key");
  if (key.empty()) return false;
  std::string accept = sha1_base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  std::ostringstream o;
  o << "HTTP/1.1 101 Switching Protocols\r\n"
       "Upgrade: websocket\r\n"
       "Connection: Upgrade\r\n"
       "Sec-WebSocket-Accept: "
    << accept << "\r\n\r\n";
  auto s = o.str();
  return write_all(fd, s.data(), s.size());
}

bool read_exact(int fd, void* data, size_t n) {
  auto* p = static_cast<char*>(data);
  size_t got_total = 0;
  while (got_total < n) {
    size_t got = 0;
    if (!read_some(fd, p + got_total, n - got_total, got)) return false;
    got_total += got;
  }
  return true;
}

bool ws_read(int fd, int& opcode, std::string& payload) {
  uint8_t hdr[2];
  if (!read_exact(fd, hdr, 2)) return false;
  opcode = hdr[0] & 0x0f;
  bool masked = (hdr[1] & 0x80) != 0;
  uint64_t len = hdr[1] & 0x7f;
  if (len == 126) {
    uint8_t ext[2];
    if (!read_exact(fd, ext, 2)) return false;
    len = (uint64_t(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8];
    if (!read_exact(fd, ext, 8)) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }
  if (len > 8ull * 1024 * 1024) return false;
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked && !read_exact(fd, mask, 4)) return false;
  payload.assign(static_cast<size_t>(len), '\0');
  if (len && !read_exact(fd, payload.data(), static_cast<size_t>(len))) return false;
  if (masked) {
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
  }
  return true;
}

bool ws_write(int fd, int opcode, const void* data, size_t n, std::mutex& mu) {
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

class SocketPeer : public WsPeer {
 public:
  explicit SocketPeer(int fd) : fd_(fd) {}
  bool send_text(const std::string& json) override { return send_frame(1, json.data(), json.size()); }
  bool send_frame(int opcode, const void* data, size_t n) { return ws_write(fd_, opcode, data, n, mu_); }

 private:
  int fd_;
  std::mutex mu_;
};

std::string adb_json(int port, const std::string& token) {
  std::string text;
  bool ok = false;
  if (find_adb().empty()) {
    text = "未找到 adb。请重新编译带 platform-tools 的尘埃X。";
  } else {
    auto devices = list_adb_devices();
    if (devices.empty() && usb_has_adb_interface()) {
      run_adb("kill-server");
      run_adb("start-server");
      devices = list_adb_devices();
    }
    std::string unauthorized;
    std::string ready;
    for (const auto& d : devices) {
      if (d.state == "unauthorized") unauthorized = d.serial;
      else if (d.state == "device" && ready.empty()) ready = d.serial;
    }
    if (!unauthorized.empty() && ready.empty()) {
      text = "手机已连上，但还没允许 USB 调试。请解锁手机，在弹窗里点「允许」。";
    } else if (ready.empty()) {
      const std::string phone = usb_phone_hint();
      if (!phone.empty()) {
        text = "USB 已插上 " + phone + "，但没有 ADB。请打开开发者选项里的「USB 调试」";
        if (phone.find("MAG") != std::string::npos || phone.find("HONOR") != std::string::npos ||
            phone.find("HUAWEI") != std::string::npos || phone.find("ALN") != std::string::npos) {
          text += "，荣耀/华为还要打开「仅充电模式下允许 ADB」";
        }
        text += "。解锁后下拉通知栏把 USB 设为「传输文件」，弹出允许调试后再点一次「准备 USB」。";
      } else {
        text = "adb 看不到手机。请用数据线连接，打开 USB 调试，并允许这台电脑。";
      }
    } else {
      std::ostringstream args;
      args << "reverse tcp:" << port << " tcp:" << port;
      text = run_adb(args.str());
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
      ok = text.find("error") == std::string::npos && text.find("not found") == std::string::npos &&
           text.find("no devices") == std::string::npos;
      if (ok) {
        std::ostringstream launch;
        launch << "shell am start -a android.intent.action.VIEW -d "
               << "'dustcam://127.0.0.1:" << port << '/' << token << "?usb=1'";
        const std::string launched = run_adb(launch.str());
        if (launched.find("Error") != std::string::npos || launched.find("error") != std::string::npos) {
          text = "已转发 " + ready + "，但没能自动打开手机 App。请在手机选 USB，填 127.0.0.1 和配对码。";
        } else {
          text = "已转发并打开手机尘埃X，不用手填地址和配对码，在手机上点「开始作为摄像头」即可。";
        }
      }
    }
  }
  std::ostringstream o;
  o << "{\"ok\":" << (ok ? "true" : "false") << ",\"message\":\"" << json_escape(text)
    << "\",\"loopback_ws\":\"ws://127.0.0.1:" << port << "/cam/ws\""
    << ",\"pair_url\":\"dustcam://127.0.0.1:" << port << '/' << json_escape(token) << "?usb=1\"}";
  return o.str();
}

std::string with_ssh(std::string json) {
  if (!json.empty() && json.back() == '}') json.pop_back();
  json += ",\"ssh\":" + ssh_host_json(ssh_host_status()) + "}";
  return json;
}

void apply_mesh_body(Mesh& mesh, const std::string& body) {
  if (body.empty()) return;
  MeshSettings s = mesh.settings();
  const std::string mode = json_get_string(body, "mode");
  if (mode == "tun") s.mode = MeshMode::Tun;
  if (mode == "tunnel") s.mode = MeshMode::Tunnel;
  const int lp = json_get_int(body, "local_port", -1);
  const int rp = json_get_int(body, "remote_port", -1);
  if (lp > 0 && lp < 65536) s.local_port = lp;
  if (rp > 0 && rp < 65536) s.remote_port = rp;
  const std::string lip = json_get_string(body, "local_ip");
  const std::string pip = json_get_string(body, "peer_ip");
  if (!lip.empty()) s.local_ip = lip;
  if (!pip.empty()) s.peer_ip = pip;
  const std::string id = json_get_string(body, "device_id");
  const std::string pw = json_get_string(body, "password");
  if (!id.empty()) s.device_id = id;
  if (!pw.empty()) s.password = pw;
  mesh.set_settings(s);
}

std::string mesh_start_json(Mesh& mesh, const std::string& body) {
  apply_mesh_body(mesh, body);
  std::string err;
  const bool proxy = mesh.start_proxy(&err);
  std::string tun_err;
  bool tun = false;
  const MeshSettings s = mesh.settings();
  if (s.mode == MeshMode::Tun) {
    tun = mesh.start_tun(s.local_ip, s.peer_ip, &tun_err);
    if (!tun && tun_err.empty()) tun_err = "虚拟网卡未启动，已回退应用层隧道。";
  } else {
    mesh.stop_tun();
  }
  std::string status = mesh.status_json();
  if (!status.empty() && status.back() == '}') status.pop_back();
  std::ostringstream o;
  o << status << ",\"proxy_ok\":" << (proxy ? "true" : "false") << ",\"tun_ok\":" << (tun ? "true" : "false");
  if (!err.empty()) o << ",\"error\":\"" << json_escape(err) << "\"";
  if (!tun_err.empty()) o << ",\"tun_error\":\"" << json_escape(tun_err) << "\"";
  o << "}";
  return with_ssh(o.str());
}

}  // namespace

Server::Server() : web_dir_(find_web_dir()) {}

Server::~Server() { stop(); }

bool Server::start() {
  net_init();
  int want = default_listen_port();
  for (int p = want; p < want + 20; ++p) {
    listen_fd_ = listen_tcp(p);
    if (listen_fd_ >= 0) {
      port_ = p;
      break;
    }
  }
  if (listen_fd_ < 0) return false;
  running_ = true;
  thread_ = std::thread([this] { accept_loop(); });
  return true;
}

void Server::stop() {
  running_ = false;
  if (listen_fd_ >= 0) {
    close_fd(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  mesh_.stop_tun();
  mesh_.stop_proxy();
  vio_.stop();
  net_shutdown();
}

void Server::accept_loop() {
  while (running_) {
    int fd = accept_fd(listen_fd_);
    if (fd < 0) {
      if (!running_) break;
      continue;
    }
    std::thread([this, fd] { handle_client(fd); }).detach();
  }
}

void Server::handle_client(int fd) {
  HttpReq req;
  if (!read_http(fd, req)) {
    close_fd(fd);
    return;
  }
  const std::string upgrade = lower(header_get(req, "upgrade"));
  if (upgrade.find("websocket") != std::string::npos) {
    if (!ws_handshake(fd, req)) {
      close_fd(fd);
      return;
    }
    if (req.path == "/cam/ws") {
      auto peer = std::make_shared<SocketPeer>(fd);
      bool attached = false;
      while (true) {
        int opcode = 0;
        std::string payload;
        if (!ws_read(fd, opcode, payload)) break;
        if (opcode == 8) break;
        if (opcode == 9) {
          peer->send_frame(10, payload.data(), payload.size());
          continue;
        }
        if (opcode != 1) continue;
        std::string type = json_get_string(payload, "type");
        if (type == "hello") {
          attached = hub_.attach(peer, json_get_string(payload, "role"), json_get_string(payload, "token"));
          if (!attached) break;
        } else if (type == "signal") {
          hub_.relay(peer, payload);
        } else {
          peer->send_text("{\"type\":\"error\",\"message\":\"unknown type\"}");
        }
      }
      if (attached) hub_.detach(peer);
      close_fd(fd);
      return;
    }
    if (req.path == "/cam/media") {
      std::mutex mu;
      while (true) {
        int opcode = 0;
        std::string payload;
        if (!ws_read(fd, opcode, payload)) break;
        if (opcode == 8) break;
        if (opcode == 9) {
          ws_write(fd, 10, payload.data(), payload.size(), mu);
          continue;
        }
        if (opcode != 2 || payload.empty()) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(payload.data());
        if (p[0] == 1 && payload.size() >= 5) {
          int w = p[1] | (p[2] << 8);
          int h = p[3] | (p[4] << 8);
          if (w > 0 && h > 0 && payload.size() >= static_cast<size_t>(5 + w * h * 3)) {
            vio_.send_rgb(p + 5, w, h);
          }
        } else if (p[0] == 2 && payload.size() >= 5) {
          uint32_t n = uint32_t(p[1]) | (uint32_t(p[2]) << 8) | (uint32_t(p[3]) << 16) | (uint32_t(p[4]) << 24);
          if (payload.size() >= 5 + n * sizeof(float)) {
            vio_.send_pcm(reinterpret_cast<const float*>(p + 5), static_cast<int>(n));
          }
        }
      }
      close_fd(fd);
      return;
    }
    if (req.path == "/mesh/bridge") {
      mesh_.attach_bridge(fd);
      std::mutex mu;
      while (true) {
        int opcode = 0;
        std::string payload;
        if (!ws_read(fd, opcode, payload)) break;
        if (opcode == 8) break;
        if (opcode == 9) {
          ws_write(fd, 10, payload.data(), payload.size(), mu);
          continue;
        }
        if (opcode != 2 || payload.empty()) continue;
        mesh_.on_bridge_bytes(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
      }
      mesh_.detach_bridge(fd);
      close_fd(fd);
      return;
    }
    close_fd(fd);
    return;
  }

  if (req.method == "OPTIONS") {
    http_reply(fd, 204, "No Content", "text/plain", "");
    close_fd(fd);
    return;
  }
  if (req.path == "/api/cam" && req.method == "GET") {
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", hub_.info_json(port_));
  } else if (req.path == "/api/cam/rotate" && req.method == "POST") {
    auto token = hub_.rotate_token();
    http_reply(fd, 200, "OK", "application/json; charset=utf-8",
               std::string("{\"ok\":true,\"token\":\"") + json_escape(token) + "\"}");
  } else if (req.path == "/api/cam/adb" && req.method == "POST") {
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", adb_json(port_, hub_.token()));
  } else if (req.path == "/api/cam/sink/start" && req.method == "POST") {
    vio_.start();
    http_reply(fd, 200, "OK", "application/json; charset=utf-8",
               std::string("{\"ok\":true,\"running\":true,\"message\":\"") + json_escape(vio_.message()) + "\"}");
  } else if (req.path == "/api/cam/sink/stop" && req.method == "POST") {
    vio_.stop();
    http_reply(fd, 200, "OK", "application/json; charset=utf-8",
               "{\"ok\":true,\"running\":false,\"message\":\"已停止虚拟设备输出\"}");
  } else if (req.path == "/api/mesh" && req.method == "GET") {
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", with_ssh(mesh_.status_json()));
  } else if (req.path == "/api/mesh" && req.method == "POST") {
    apply_mesh_body(mesh_, req.body);
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", with_ssh(mesh_.status_json()));
  } else if (req.path == "/api/mesh/start" && req.method == "POST") {
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", mesh_start_json(mesh_, req.body));
  } else if (req.path == "/api/mesh/stop" && req.method == "POST") {
    mesh_.stop_tun();
    mesh_.stop_proxy();
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", with_ssh(mesh_.status_json()));
  } else if (req.path == "/api/ssh/enable" && req.method == "POST") {
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", ssh_host_json(ssh_host_enable()));
  } else if (req.path == "/api/mesh/tun/start" && req.method == "POST") {
    apply_mesh_body(mesh_, req.body);
    const MeshSettings s = mesh_.settings();
    std::string err;
    const bool ok = mesh_.start_tun(s.local_ip, s.peer_ip, &err);
    std::string status = mesh_.status_json();
    if (!status.empty() && status.front() == '{') status.erase(status.begin());
    std::ostringstream o;
    o << "{\"ok\":" << (ok ? "true" : "false");
    if (!err.empty()) o << ",\"error\":\"" << json_escape(err) << "\"";
    o << "," << status;
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", o.str());
  } else if (req.path == "/api/mesh/tun/stop" && req.method == "POST") {
    mesh_.stop_tun();
    http_reply(fd, 200, "OK", "application/json; charset=utf-8", with_ssh(mesh_.status_json()));
  } else if (req.method == "GET") {
    std::string rel = req.path;
    if (rel == "/") rel = "/cam.html";
    if (rel.find("..") != std::string::npos) {
      http_reply(fd, 400, "Bad Request", "text/plain", "bad path");
    } else {
      std::string body = read_file(web_dir_ + rel);
      if (body.empty()) {
        http_reply(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found");
      } else {
        http_reply(fd, 200, "OK", mime_for(rel), body);
      }
    }
  } else {
    http_reply(fd, 404, "Not Found", "text/plain", "not found");
  }
  close_fd(fd);
}

}  // namespace dustx
