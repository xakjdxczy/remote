#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace dustx {

void net_init();
void net_shutdown();
void close_fd(int fd);
void set_tcp_nodelay(int fd);
int listen_tcp(int port);
int listen_tcp_loopback(int port);
int connect_loopback(int port);
int accept_fd(int listen_fd);
bool ws_write_frame(int fd, int opcode, const void* data, size_t n, std::mutex& mu);
bool ws_read_frame(int fd, int& opcode, std::string& payload);
std::vector<std::string> lan_ipv4s();
std::string run_command(const std::string& cmd);
std::string find_adb();
std::string run_adb(const std::string& args);
struct AdbDevice {
  std::string serial;
  std::string state;
};
std::vector<AdbDevice> list_adb_devices();
std::string usb_phone_hint();
bool usb_has_adb_interface();

}  // namespace dustx
