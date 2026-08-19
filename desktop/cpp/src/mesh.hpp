#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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

  bool start_tun(const std::string& local_ip, const std::string& peer_ip, std::string* err);
  void stop_tun();
  bool tun_running() const;
  static bool tun_available(std::string* reason);

  void attach_bridge(int fd);
  void detach_bridge(int fd);
  void on_bridge_bytes(const uint8_t* data, size_t n);

 private:
  void accept_loop();
  void pump_stream(uint32_t id, int fd);
  void handle_open(uint32_t id);
  void handle_data(uint32_t id, const uint8_t* p, size_t n);
  void handle_close(uint32_t id);
  void send_frame(uint8_t type, uint32_t id, const void* data, size_t n);
  void close_stream_locked(uint32_t id);
  void spawn_pump(uint32_t id, int fd);
  void wait_pumps();

  mutable std::mutex mu_;
  std::mutex bridge_write_;
  MeshSettings settings_;
  int listen_fd_ = -1;
  int bridge_fd_ = -1;
  bool proxy_on_ = false;
  bool tun_on_ = false;
  uint32_t next_id_ = 1;
  std::thread accept_th_;
  std::atomic<int> pumps_alive_{0};
  std::unordered_map<uint32_t, int> fds_;
};

std::string mesh_config_path();
MeshSettings load_mesh_settings();
void save_mesh_settings(const MeshSettings& s);

}  // namespace dustx
