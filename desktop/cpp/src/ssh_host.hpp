#pragma once

#include <string>

namespace dustx {

struct SshHostStatus {
  bool ok = true;
  bool ready = false;
  bool can_enable = false;
  bool need_admin = false;
  std::string os;
  std::string username;
  std::string message;
};

SshHostStatus ssh_host_status();
SshHostStatus ssh_host_enable();
std::string ssh_host_json(const SshHostStatus& s);

}  // namespace dustx
