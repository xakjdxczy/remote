#include "ssh_host.hpp"

#include "net.hpp"

#include <pwd.h>
#include <unistd.h>

namespace dustx {
namespace {

std::string current_user() {
  const passwd* pw = getpwuid(getuid());
  if (!pw || !pw->pw_name) return {};
  return pw->pw_name;
}

bool port_open(int port) {
  const int fd = connect_loopback(port);
  if (fd < 0) return false;
  close_fd(fd);
  return true;
}

}  // namespace

SshHostStatus ssh_host_status() {
  SshHostStatus s;
  s.os = "macos";
  s.username = current_user();
  s.can_enable = false;
  s.ready = port_open(22);
  if (s.ready) {
    s.message = "本机已开启远程登录。若要连 Windows，请让 Windows 打开尘埃X，点「开启本机 SSH」。";
  } else {
    s.message =
        "连 Windows 时，在 Windows 的尘埃X 里点「开启本机 SSH」。连上后在这台 Mac 的终端登录，不用在这里装服务。";
  }
  return s;
}

SshHostStatus ssh_host_enable() {
  SshHostStatus s = ssh_host_status();
  s.ok = false;
  s.message = "Mac 请在「系统设置 → 通用 → 共享 → 远程登录」打开。连 Windows 则在 Windows 尘埃X 里一键开启。";
  return s;
}

}  // namespace dustx
