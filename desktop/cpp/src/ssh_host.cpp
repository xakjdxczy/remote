#include "ssh_host.hpp"

#include "log.hpp"
#include "util.hpp"

#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>

namespace dustx {
namespace {

std::atomic<bool> g_busy{false};
std::mutex g_last_mu;
SshHostStatus g_last;

}  // namespace

std::string ssh_host_json(const SshHostStatus& s) {
  std::ostringstream o;
  o << "{\"ok\":" << (s.ok ? "true" : "false")
    << ",\"ready\":" << (s.ready ? "true" : "false")
    << ",\"can_enable\":" << (s.can_enable ? "true" : "false")
    << ",\"need_admin\":" << (s.need_admin ? "true" : "false")
    << ",\"busy\":" << (s.busy ? "true" : "false")
    << ",\"os\":\"" << json_escape(s.os) << "\""
    << ",\"username\":\"" << json_escape(s.username) << "\""
    << ",\"message\":\"" << json_escape(s.message) << "\"}";
  return o.str();
}

SshHostStatus ssh_host_status() {
  SshHostStatus s = ssh_host_probe();
  s.busy = g_busy.load();
  if (s.busy) {
    s.message = "正在开启本机 SSH，进度见下方运行日志。";
  }
  return s;
}

void ssh_host_start_enable() {
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) {
    log_info("ssh", "开启任务已在进行，忽略重复点击");
    return;
  }
  log_info("ssh", "收到开启本机 SSH 请求");
  std::thread([] {
    SshHostStatus st;
    try {
      st = ssh_host_enable_work();
    } catch (...) {
      st.ok = false;
      st.message = "开启 SSH 时发生异常。";
      log_error("ssh", st.message);
    }
    st.busy = false;
    {
      std::lock_guard<std::mutex> lock(g_last_mu);
      g_last = st;
    }
    g_busy = false;
    log_info("ssh", std::string("开启结束：") + (st.ok && st.ready ? "成功" : "失败") + " " + st.message);
  }).detach();
}

}  // namespace dustx
