#include "ssh_host.hpp"

#include "util.hpp"

#include <sstream>

namespace dustx {

std::string ssh_host_json(const SshHostStatus& s) {
  std::ostringstream o;
  o << "{\"ok\":" << (s.ok ? "true" : "false")
    << ",\"ready\":" << (s.ready ? "true" : "false")
    << ",\"can_enable\":" << (s.can_enable ? "true" : "false")
    << ",\"need_admin\":" << (s.need_admin ? "true" : "false")
    << ",\"os\":\"" << json_escape(s.os) << "\""
    << ",\"username\":\"" << json_escape(s.username) << "\""
    << ",\"message\":\"" << json_escape(s.message) << "\"}";
  return o.str();
}

}  // namespace dustx
