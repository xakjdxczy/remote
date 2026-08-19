#include "mesh.hpp"

#include "mesh_tun.hpp"
#include "net.hpp"
#include "util.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
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

constexpr uint8_t kOpen = 1;
constexpr uint8_t kData = 2;
constexpr uint8_t kClose = 3;
constexpr uint8_t kTun = 4;

void put_u32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[3] = static_cast<uint8_t>(v & 0xff);
}

uint32_t get_u32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void mkdir_p(const std::string& dir) {
#ifdef _WIN32
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '\\' || dir[i] == '/' || i + 1 == dir.size()) {
      _mkdir(cur.c_str());
    }
  }
#else
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '/' || i + 1 == dir.size()) {
      mkdir(cur.c_str(), 0755);
    }
  }
#endif
}

}  // namespace

std::string mesh_config_path() {
#ifdef _WIN32
  wchar_t appdata[MAX_PATH];
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return "dustx-mesh.json";
  return wide_to_utf8(appdata) + "\\DustX\\mesh.json";
#else
  const char* home = std::getenv("HOME");
  if (!home || !*home) return "dustx-mesh.json";
  return std::string(home) + "/Library/Application Support/DustX/mesh.json";
#endif
}

MeshSettings load_mesh_settings() {
  MeshSettings s;
  const std::string raw = read_file(mesh_config_path());
  if (raw.empty()) return s;
  const std::string mode = json_get_string(raw, "mode");
  if (mode == "tun") s.mode = MeshMode::Tun;
  if (mode == "tunnel") s.mode = MeshMode::Tunnel;
  const int lp = json_get_int(raw, "local_port", s.local_port);
  const int rp = json_get_int(raw, "remote_port", s.remote_port);
  if (lp > 0 && lp < 65536) s.local_port = lp;
  if (rp > 0 && rp < 65536) s.remote_port = rp;
  const std::string lip = json_get_string(raw, "local_ip");
  const std::string pip = json_get_string(raw, "peer_ip");
  if (!lip.empty()) s.local_ip = lip;
  if (!pip.empty()) s.peer_ip = pip;
  s.device_id = json_get_string(raw, "device_id");
  s.password = json_get_string(raw, "password");
  return s;
}

void save_mesh_settings(const MeshSettings& s) {
  const std::string path = mesh_config_path();
  const auto slash = path.find_last_of("/\\");
  if (slash != std::string::npos) mkdir_p(path.substr(0, slash));
  std::ofstream out(path, std::ios::binary);
  out << "{\"mode\":\"" << (s.mode == MeshMode::Tun ? "tun" : "tunnel") << "\""
      << ",\"local_port\":" << s.local_port << ",\"remote_port\":" << s.remote_port
      << ",\"local_ip\":\"" << json_escape(s.local_ip) << "\",\"peer_ip\":\"" << json_escape(s.peer_ip) << "\""
      << ",\"device_id\":\"" << json_escape(s.device_id) << "\",\"password\":\"" << json_escape(s.password) << "\"}\n";
}

Mesh::Mesh() : settings_(load_mesh_settings()) {
  std::random_device rd;
  next_id_ = (static_cast<uint32_t>(rd()) & 0x7fffu) << 16 | 1u;
}

Mesh::~Mesh() {
  stop_tun();
  stop_proxy();
}

MeshSettings Mesh::settings() const {
  std::lock_guard<std::mutex> lock(mu_);
  return settings_;
}

void Mesh::set_settings(const MeshSettings& s) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    settings_ = s;
  }
  save_mesh_settings(s);
}

bool Mesh::proxy_running() const {
  std::lock_guard<std::mutex> lock(mu_);
  return proxy_on_;
}

bool Mesh::tun_running() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tun_on_;
}

bool Mesh::tun_available(std::string* reason) { return platform_tun_available(reason); }

std::string Mesh::status_json() const {
  MeshSettings s;
  bool proxy = false;
  bool tun = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    s = settings_;
    proxy = proxy_on_;
    tun = tun_on_;
  }
  std::string reason;
  const bool can_tun = platform_tun_available(&reason);
  std::ostringstream o;
  o << "{\"ok\":true,\"mode\":\"" << (s.mode == MeshMode::Tun ? "tun" : "tunnel") << "\""
    << ",\"local_port\":" << s.local_port << ",\"remote_port\":" << s.remote_port
    << ",\"local_ip\":\"" << json_escape(s.local_ip) << "\",\"peer_ip\":\"" << json_escape(s.peer_ip) << "\""
    << ",\"device_id\":\"" << json_escape(s.device_id) << "\",\"password\":\"" << json_escape(s.password) << "\""
    << ",\"proxy_running\":" << (proxy ? "true" : "false") << ",\"tun_running\":" << (tun ? "true" : "false")
    << ",\"tun_available\":" << (can_tun ? "true" : "false") << ",\"tun_reason\":\"" << json_escape(reason) << "\""
    << ",\"signal_ws\":\"" << json_escape(signaling_ws_url()) << "\""
    << ",\"signal_http\":\"" << json_escape(signaling_http_origin()) << "\""
    << ",\"apple_default\":\"tunnel\",\"ice_servers\":" << ice_servers_json() << "}";
  return o.str();
}

bool Mesh::start_proxy(std::string* err) {
  stop_proxy();
  int port = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    port = settings_.local_port;
  }
  const int fd = listen_tcp_loopback(port);
  if (fd < 0) {
    if (err) *err = "无法在 127.0.0.1 监听本地端口，换一个端口再试。";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    listen_fd_ = fd;
    proxy_on_ = true;
  }
  accept_th_ = std::thread([this] { accept_loop(); });
  return true;
}

void Mesh::spawn_pump(uint32_t id, int fd) {
  pumps_alive_++;
  std::thread([this, id, fd] {
    pump_stream(id, fd);
    pumps_alive_--;
  }).detach();
}

void Mesh::wait_pumps() {
  for (int i = 0; i < 80 && pumps_alive_.load() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

void Mesh::stop_proxy() {
  int lfd = -1;
  std::unordered_map<uint32_t, int> fds;
  {
    std::lock_guard<std::mutex> lock(mu_);
    proxy_on_ = false;
    lfd = listen_fd_;
    listen_fd_ = -1;
    fds.swap(fds_);
  }
  if (lfd >= 0) close_fd(lfd);
  for (auto& kv : fds) close_fd(kv.second);
  if (accept_th_.joinable()) accept_th_.join();
  wait_pumps();
}

void Mesh::accept_loop() {
  while (true) {
    int lfd = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!proxy_on_) break;
      lfd = listen_fd_;
    }
    if (lfd < 0) break;
    const int c = accept_fd(lfd);
    if (c < 0) {
      std::lock_guard<std::mutex> lock(mu_);
      if (!proxy_on_) break;
      continue;
    }
    set_tcp_nodelay(c);
    uint32_t id = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      id = next_id_++;
      fds_[id] = c;
    }
    spawn_pump(id, c);
    send_frame(kOpen, id, nullptr, 0);
  }
}

void Mesh::pump_stream(uint32_t id, int fd) {
  char buf[8192];
  while (true) {
    size_t got = 0;
    if (!read_some(fd, buf, sizeof(buf), got)) break;
    send_frame(kData, id, buf, got);
  }
  send_frame(kClose, id, nullptr, 0);
  std::lock_guard<std::mutex> lock(mu_);
  close_stream_locked(id);
}

void Mesh::handle_open(uint32_t id) {
  int port = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    port = settings_.remote_port;
    if (fds_.count(id)) return;
  }
  const int fd = connect_loopback(port);
  if (fd < 0) {
    send_frame(kClose, id, nullptr, 0);
    return;
  }
  set_tcp_nodelay(fd);
  {
    std::lock_guard<std::mutex> lock(mu_);
    fds_[id] = fd;
  }
  spawn_pump(id, fd);
}

void Mesh::handle_data(uint32_t id, const uint8_t* p, size_t n) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = fds_.find(id);
    if (it != fds_.end()) fd = it->second;
  }
  if (fd >= 0) write_all(fd, p, n);
}

void Mesh::handle_close(uint32_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  close_stream_locked(id);
}

void Mesh::close_stream_locked(uint32_t id) {
  const auto it = fds_.find(id);
  if (it == fds_.end()) return;
  close_fd(it->second);
  fds_.erase(it);
}

void Mesh::send_frame(uint8_t type, uint32_t id, const void* data, size_t n) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fd = bridge_fd_;
  }
  if (fd < 0) return;
  std::vector<uint8_t> buf(9 + n);
  buf[0] = type;
  put_u32(buf.data() + 1, id);
  put_u32(buf.data() + 5, static_cast<uint32_t>(n));
  if (n && data) std::memcpy(buf.data() + 9, data, n);
  ws_write_frame(fd, 2, buf.data(), buf.size(), bridge_write_);
}

void Mesh::on_bridge_bytes(const uint8_t* data, size_t n) {
  size_t off = 0;
  while (off + 9 <= n) {
    const uint8_t type = data[off];
    const uint32_t id = get_u32(data + off + 1);
    const uint32_t len = get_u32(data + off + 5);
    if (off + 9 + len > n) break;
    const uint8_t* payload = data + off + 9;
    if (type == kOpen) handle_open(id);
    else if (type == kData) handle_data(id, payload, len);
    else if (type == kClose) handle_close(id);
    else if (type == kTun) platform_tun_inject(payload, len);
    off += 9 + len;
  }
}

void Mesh::attach_bridge(int fd) {
  std::lock_guard<std::mutex> lock(mu_);
  bridge_fd_ = fd;
}

void Mesh::detach_bridge(int fd) {
  std::lock_guard<std::mutex> lock(mu_);
  if (bridge_fd_ == fd) bridge_fd_ = -1;
}

bool Mesh::start_tun(const std::string& local_ip, const std::string& peer_ip, std::string* err) {
  stop_tun();
  std::string reason;
  if (!platform_tun_available(&reason)) {
    if (err) *err = reason;
    return false;
  }
  std::string lip = local_ip;
  std::string pip = peer_ip;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (lip.empty()) lip = settings_.local_ip;
    if (pip.empty()) pip = settings_.peer_ip;
    if (!local_ip.empty()) settings_.local_ip = local_ip;
    if (!peer_ip.empty()) settings_.peer_ip = peer_ip;
  }
  const bool ok = platform_tun_start(lip, pip, [this](const uint8_t* pkt, size_t n) { send_frame(kTun, 0, pkt, n); }, err);
  {
    std::lock_guard<std::mutex> lock(mu_);
    tun_on_ = ok;
    if (ok) save_mesh_settings(settings_);
  }
  return ok;
}

void Mesh::stop_tun() {
  platform_tun_stop();
  std::lock_guard<std::mutex> lock(mu_);
  tun_on_ = false;
}

}  // namespace dustx
