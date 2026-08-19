#include "mesh_tun.hpp"

namespace dustx {

bool platform_tun_available(std::string* reason) {
  if (reason) {
    *reason =
        "macOS 虚拟网卡需要项目自己的付费 Apple 账号开通 Packet Tunnel Network Extension。"
        "当前开源 / ad-hoc 签名无法激活，请用应用层隧道（127.0.0.1 端口转发）。";
  }
  return false;
}

bool platform_tun_start(const std::string&, const std::string&, TunSend, std::string* err) {
  if (err) {
    *err =
        "未启用 macOS Packet Tunnel。没有项目自己的 Network Extension 许可时不能装虚拟网卡，"
        "请改用应用层隧道。";
  }
  return false;
}

void platform_tun_stop() {}

bool platform_tun_inject(const uint8_t*, size_t) { return false; }

}  // namespace dustx
