#pragma once

#include "cam_hub.hpp"
#include "mesh.hpp"
#include "virtual_io.hpp"

#include <atomic>
#include <string>
#include <thread>

namespace dustx {

class Server {
 public:
  Server();
  ~Server();
  bool start();
  void stop();
  int port() const { return port_; }
  CamHub& hub() { return hub_; }
  Mesh& mesh() { return mesh_; }
  VirtualIO& virtual_io() { return vio_; }
  const std::string& web_dir() const { return web_dir_; }
  const std::string& remote_ui_dir() const { return remote_ui_dir_; }
  bool has_shell_ui() const;

 private:
  void accept_loop();
  void handle_client(int fd);

  int port_ = 0;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
  CamHub hub_;
  Mesh mesh_;
  VirtualIO vio_;
  std::string web_dir_;
  std::string remote_ui_dir_;
};

}  // namespace dustx
