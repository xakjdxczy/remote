#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dustx {

enum class MeshMode { Tunnel, Tun };

struct MeshSettings {
  MeshMode mode = MeshMode::Tunnel;
  int local_port = 2222;
  int remote_port = 22;
  std::string local_ip = "100.64.0.1";
  std::string peer_ip = "100.64.0.2";
  std::string device_id;
  std::string password;
  std::string ssh_user;
};

struct MeshLink {
  std::string key;
  int local_port = 0;
  int listen_fd = -1;
  int bridge_fd = -1;
  bool proxy_on = false;
  uint32_t next_id = 1;
  std::thread accept_th;
  std::unordered_map<uint32_t, int> fds;
};

class Mesh {
 public:
  Mesh();
  ~Mesh();

  MeshSettings settings() const;
  void set_settings(const MeshSettings& s);
  std::string status_json() const;

  bool start_proxy(std::string* err);
  void stop_proxy();
  bool proxy_running() const;

  bool add_peer(const std::string& key, int local_port, std::string* err);
  void remove_peer(const std::string& key);

  bool start_tun(const std::string& local_ip, const std::string& peer_ip, std::string* err);
  void stop_tun();
  bool tun_running() const;
  static bool tun_available(std::string* reason);

  void attach_bridge(int fd, const std::string& key);
  void detach_bridge(int fd);
  void on_bridge_bytes(int fd, const uint8_t* data, size_t n);

 private:
  void accept_loop(MeshLink* link);
  void pump_stream(MeshLink* link, uint32_t id, int sock);
  void handle_open(MeshLink* link, uint32_t id);
  void handle_data(MeshLink* link, uint32_t id, const uint8_t* p, size_t n);
  void handle_close(MeshLink* link, uint32_t id);
  void send_frame(MeshLink* link, uint8_t type, uint32_t id, const void* data, size_t n);
  void close_stream_locked(MeshLink* link, uint32_t id);
  void spawn_pump(MeshLink* link, uint32_t id, int sock);
  void wait_pumps();
  void stop_link_locked(MeshLink* link);
  MeshLink* link_by_fd_locked(int fd);
  MeshLink* link_by_key_locked(const std::string& key);

  mutable std::mutex mu_;
  std::mutex bridge_write_;
  MeshSettings settings_;
  bool tun_on_ = false;
  std::atomic<int> pumps_alive_{0};
  std::unordered_map<std::string, std::unique_ptr<MeshLink>> links_;
};

std::string mesh_config_path();
MeshSettings load_mesh_settings();
void save_mesh_settings(const MeshSettings& s);

}  // namespace dustx
