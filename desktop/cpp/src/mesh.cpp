#include "mesh.hpp"

#include "device_fp.hpp"
#include "device_info.hpp"
#include "log.hpp"
#include "mesh_tun.hpp"
#include "net.hpp"
#include "recents.hpp"
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
  return dustx_config_dir() + "\\mesh.json";
#else
  return dustx_config_dir() + "/mesh.json";
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
  s.ssh_user = json_get_string(raw, "ssh_user");
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
      << ",\"device_id\":\"" << json_escape(s.device_id) << "\""
      << ",\"ssh_user\":\"" << json_escape(s.ssh_user) << "\"}\n";
}

Mesh::Mesh() : settings_(load_mesh_settings()) {
  const std::string raw = read_file(mesh_config_path());
  if (raw.find("\"password\"") != std::string::npos) save_mesh_settings(settings_);
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
  for (const auto& kv : links_) {
    if (kv.second && kv.second->proxy_on) return true;
  }
  return false;
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
  std::vector<std::pair<std::string, int>> peers;
  {
    std::lock_guard<std::mutex> lock(mu_);
    s = settings_;
    tun = tun_on_;
    for (const auto& kv : links_) {
      if (!kv.second) continue;
      if (kv.second->proxy_on) proxy = true;
      peers.emplace_back(kv.first, kv.second->local_port);
    }
  }
  std::string reason;
  const bool can_tun = platform_tun_available(&reason);
  std::ostringstream o;
  o << "{\"ok\":true,\"mode\":\"" << (s.mode == MeshMode::Tun ? "tun" : "tunnel") << "\""
    << ",\"local_port\":" << s.local_port << ",\"remote_port\":" << s.remote_port
    << ",\"local_ip\":\"" << json_escape(s.local_ip) << "\",\"peer_ip\":\"" << json_escape(s.peer_ip) << "\""
    << ",\"device_id\":\"" << json_escape(s.device_id) << "\",\"ssh_user\":\"" << json_escape(s.ssh_user) << "\""
    << ",\"proxy_running\":" << (proxy ? "true" : "false") << ",\"tun_running\":" << (tun ? "true" : "false")
    << ",\"tun_available\":" << (can_tun ? "true" : "false") << ",\"tun_reason\":\"" << json_escape(reason) << "\""
    << ",\"signal_ws\":\"" << json_escape(signaling_ws_url()) << "\""
    << ",\"signal_http\":\"" << json_escape(signaling_http_origin()) << "\""
    << ",\"apple_default\":\"tunnel\",\"ice_servers\":" << ice_servers_json()
    << ",\"fingerprint\":" << hardware_fingerprint().json()
    << ",\"info\":" << device_info_json()
    << ",\"peers\":[";
  for (size_t i = 0; i < peers.size(); ++i) {
    if (i) o << ',';
    o << "{\"key\":\"" << json_escape(peers[i].first) << "\",\"local_port\":" << peers[i].second << "}";
  }
  o << "]}";
  return o.str();
}

MeshLink* Mesh::link_by_key_locked(const std::string& key) {
  const auto it = links_.find(key.empty() ? "default" : key);
  return it == links_.end() ? nullptr : it->second.get();
}

MeshLink* Mesh::link_by_fd_locked(int fd) {
  for (auto& kv : links_) {
    if (kv.second && kv.second->bridge_fd == fd) return kv.second.get();
  }
  return nullptr;
}

void Mesh::stop_link_locked(MeshLink* link) {
  if (!link) return;
  link->proxy_on = false;
  if (link->listen_fd >= 0) {
    close_fd(link->listen_fd);
    link->listen_fd = -1;
  }
  for (auto& kv : link->fds) close_fd(kv.second);
  link->fds.clear();
}

bool Mesh::add_peer(const std::string& key, int local_port, std::string* err) {
  const std::string id = key.empty() ? "default" : key;
  if (local_port <= 0 || local_port > 65535) {
    if (err) *err = "本地端口无效";
    return false;
  }
  MeshLink* existing = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    existing = link_by_key_locked(id);
    if (existing && existing->proxy_on && existing->local_port == local_port) return true;
  }
  if (existing) remove_peer(id);

  const int fd = listen_tcp_loopback(local_port);
  if (fd < 0) {
    log_error("mesh", "无法在 127.0.0.1:" + std::to_string(local_port) + " 监听本地端口");
    if (err) *err = "无法在 127.0.0.1 监听本地端口，换一个端口再试。";
    return false;
  }
  log_info("mesh", "互访隧道 " + id.substr(0, 8) + " 监听 127.0.0.1:" + std::to_string(local_port));
  auto link = std::make_unique<MeshLink>();
  link->key = id;
  link->local_port = local_port;
  link->listen_fd = fd;
  link->proxy_on = true;
  link->next_id = (static_cast<uint32_t>(std::random_device{}()) & 0x7fffu) << 16 | 1u;
  MeshLink* raw = link.get();
  {
    std::lock_guard<std::mutex> lock(mu_);
    links_[id] = std::move(link);
  }
  raw->accept_th = std::thread([this, raw] { accept_loop(raw); });
  return true;
}

void Mesh::remove_peer(const std::string& key) {
  const std::string id = key.empty() ? "default" : key;
  std::unique_ptr<MeshLink> owned;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = links_.find(id);
    if (it == links_.end()) return;
    stop_link_locked(it->second.get());
    owned = std::move(it->second);
    links_.erase(it);
  }
  if (owned && owned->accept_th.joinable()) owned->accept_th.join();
  wait_pumps();
}

bool Mesh::start_proxy(std::string* err) {
  int port = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    port = settings_.local_port;
  }
  return add_peer("default", port, err);
}

void Mesh::spawn_pump(MeshLink* link, uint32_t id, int sock) {
  pumps_alive_++;
  std::thread([this, link, id, sock] {
    pump_stream(link, id, sock);
    pumps_alive_--;
  }).detach();
}

void Mesh::wait_pumps() {
  for (int i = 0; i < 80 && pumps_alive_.load() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

void Mesh::stop_proxy() {
  std::vector<std::unique_ptr<MeshLink>> owned;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& kv : links_) {
      stop_link_locked(kv.second.get());
      owned.push_back(std::move(kv.second));
    }
    links_.clear();
  }
  for (auto& link : owned) {
    if (link && link->accept_th.joinable()) link->accept_th.join();
  }
  wait_pumps();
}

void Mesh::accept_loop(MeshLink* link) {
  while (true) {
    int lfd = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!link->proxy_on) break;
      lfd = link->listen_fd;
    }
    if (lfd < 0) break;
    const int c = accept_fd(lfd);
    if (c < 0) {
      std::lock_guard<std::mutex> lock(mu_);
      if (!link->proxy_on) break;
      continue;
    }
    set_tcp_nodelay(c);
    uint32_t id = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      id = link->next_id++;
      link->fds[id] = c;
    }
    spawn_pump(link, id, c);
    send_frame(link, kOpen, id, nullptr, 0);
  }
}

void Mesh::pump_stream(MeshLink* link, uint32_t id, int sock) {
  char buf[8192];
  while (true) {
    size_t got = 0;
    if (!read_some(sock, buf, sizeof(buf), got)) break;
    send_frame(link, kData, id, buf, got);
  }
  send_frame(link, kClose, id, nullptr, 0);
  std::lock_guard<std::mutex> lock(mu_);
  close_stream_locked(link, id);
}

void Mesh::handle_open(MeshLink* link, uint32_t id) {
  int port = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    port = settings_.remote_port;
    if (link->fds.count(id)) return;
  }
  const int fd = connect_loopback(port);
  if (fd < 0) {
    log_warn("mesh", "对端 OPEN 后无法连接本机 127.0.0.1:" + std::to_string(port));
    send_frame(link, kClose, id, nullptr, 0);
    return;
  }
  log_info("mesh", "已把流 " + std::to_string(id) + " 接到 127.0.0.1:" + std::to_string(port));
  set_tcp_nodelay(fd);
  {
    std::lock_guard<std::mutex> lock(mu_);
    link->fds[id] = fd;
  }
  spawn_pump(link, id, fd);
}

void Mesh::handle_data(MeshLink* link, uint32_t id, const uint8_t* p, size_t n) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = link->fds.find(id);
    if (it != link->fds.end()) fd = it->second;
  }
  if (fd >= 0) write_all(fd, p, n);
}

void Mesh::handle_close(MeshLink* link, uint32_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  close_stream_locked(link, id);
}

void Mesh::close_stream_locked(MeshLink* link, uint32_t id) {
  if (!link) return;
  const auto it = link->fds.find(id);
  if (it == link->fds.end()) return;
  close_fd(it->second);
  link->fds.erase(it);
}

void Mesh::send_frame(MeshLink* link, uint8_t type, uint32_t id, const void* data, size_t n) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (link) fd = link->bridge_fd;
  }
  if (fd < 0) return;
  std::vector<uint8_t> buf(9 + n);
  buf[0] = type;
  put_u32(buf.data() + 1, id);
  put_u32(buf.data() + 5, static_cast<uint32_t>(n));
  if (n && data) std::memcpy(buf.data() + 9, data, n);
  ws_write_frame(fd, 2, buf.data(), buf.size(), bridge_write_);
}

void Mesh::on_bridge_bytes(int fd, const uint8_t* data, size_t n) {
  MeshLink* link = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    link = link_by_fd_locked(fd);
  }
  if (!link) return;
  size_t off = 0;
  while (off + 9 <= n) {
    const uint8_t type = data[off];
    const uint32_t id = get_u32(data + off + 1);
    const uint32_t len = get_u32(data + off + 5);
    if (off + 9 + len > n) break;
    const uint8_t* payload = data + off + 9;
    if (type == kOpen) handle_open(link, id);
    else if (type == kData) handle_data(link, id, payload, len);
    else if (type == kClose) handle_close(link, id);
    else if (type == kTun) platform_tun_inject(payload, len);
    off += 9 + len;
  }
}

void Mesh::attach_bridge(int fd, const std::string& key) {
  const std::string id = key.empty() ? "default" : key;
  std::lock_guard<std::mutex> lock(mu_);
  MeshLink* link = link_by_key_locked(id);
  if (!link) {
    auto created = std::make_unique<MeshLink>();
    created->key = id;
    link = created.get();
    links_[id] = std::move(created);
  }
  link->bridge_fd = fd;
}

void Mesh::detach_bridge(int fd) {
  std::lock_guard<std::mutex> lock(mu_);
  MeshLink* link = link_by_fd_locked(fd);
  if (link && link->bridge_fd == fd) link->bridge_fd = -1;
}

bool Mesh::start_tun(const std::string& local_ip, const std::string& peer_ip, std::string* err) {
  stop_tun();
  std::string reason;
  if (!platform_tun_available(&reason)) {
    log_warn("mesh", reason);
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
  const bool ok = platform_tun_start(
      lip, pip,
      [this](const uint8_t* pkt, size_t n) {
        MeshLink* link = nullptr;
        {
          std::lock_guard<std::mutex> lock(mu_);
          link = link_by_key_locked("default");
        }
        if (link) send_frame(link, kTun, 0, pkt, n);
      },
      err);
  log_info("mesh", std::string("虚拟网卡启动 ") + (ok ? "成功" : "失败") + " " + lip + " -> " + pip +
                       (err && !err->empty() ? " " + *err : ""));
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
