#include "cam_hub.hpp"

#include "log.hpp"
#include "net.hpp"
#include "util.hpp"

#include <sstream>

namespace dustx {

CamHub::CamHub() : token_(make_token()) {}

std::string CamHub::token() const {
  std::lock_guard<std::mutex> lock(mu_);
  return token_;
}

std::string CamHub::rotate_token() {
  std::lock_guard<std::mutex> lock(mu_);
  token_ = make_token();
  return token_;
}

std::string CamHub::info_json(int port) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto ips = lan_ipv4s();
  std::ostringstream o;
  o << "{\"ok\":true,\"token\":\"" << json_escape(token_) << "\",\"port\":" << port << ",\"ips\":[";
  for (size_t i = 0; i < ips.size(); ++i) {
    if (i) o << ',';
    o << '"' << json_escape(ips[i]) << '"';
  }
  o << "],\"http_urls\":[";
  for (size_t i = 0; i < ips.size(); ++i) {
    if (i) o << ',';
    o << "\"http://" << json_escape(ips[i]) << ':' << port << "/\"";
  }
  o << "],\"ws_urls\":[";
  for (size_t i = 0; i < ips.size(); ++i) {
    if (i) o << ',';
    o << "\"ws://" << json_escape(ips[i]) << ':' << port << "/cam/ws\"";
  }
  o << "],\"pair_urls\":[";
  for (size_t i = 0; i < ips.size(); ++i) {
    if (i) o << ',';
    o << "\"dustcam://" << json_escape(ips[i]) << ':' << port << '/' << json_escape(token_) << '"';
  }
  o << "],\"ice_servers\":[{\"urls\":["
       "\"stun:stun.l.google.com:19302\","
       "\"stun:stun.qq.com:3478\","
       "\"stun:stun.miwifi.com:3478\","
       "\"stun:stun.cloudflare.com:3478\""
       "]}],\"usb\":{"
    << "\"loopback_ws\":\"ws://127.0.0.1:" << port << "/cam/ws\","
    << "\"pair_url\":\"dustcam://127.0.0.1:" << port << '/' << json_escape(token_) << "?usb=1\","
    << "\"adb_reverse\":\"adb reverse tcp:" << port << " tcp:" << port << "\","
    << "\"hint\":\"USB 线本身不是网络。adb 打开「USB 调试」后点「准备 USB」，电脑会自动打开手机并填好地址和配对码。"
       "不想开调试：改用手机的 USB 网络共享，填电脑在那条 USB 网上的 IP。\""
    << "}}";
  return o.str();
}

bool CamHub::attach(const std::shared_ptr<WsPeer>& peer, const std::string& role, const std::string& token) {
  std::shared_ptr<WsPeer> old;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (token != token_) {
      log_warn("cam", "配对码错误 role=" + role);
      peer->send_text("{\"type\":\"error\",\"message\":\"配对码错误\"}");
      return false;
    }
    if (role == "desktop") {
      if (desktop_ && desktop_ != peer) old = desktop_;
      desktop_ = peer;
    } else if (role == "phone") {
      if (phone_ && phone_ != peer) old = phone_;
      phone_ = peer;
    } else {
      peer->send_text("{\"type\":\"error\",\"message\":\"role 必须是 desktop 或 phone\"}");
      return false;
    }
  }
  if (old) {
    old->send_text("{\"type\":\"replaced\"}");
  }
  peer->send_text(std::string("{\"type\":\"hello_ok\",\"role\":\"") + json_escape(role) + "\"}");
  log_info("cam", "已接入 " + role);
  broadcast_ready();
  return true;
}

void CamHub::detach(const std::shared_ptr<WsPeer>& peer) {
  std::shared_ptr<WsPeer> other;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (desktop_ == peer) {
      desktop_.reset();
      other = phone_;
    } else if (phone_ == peer) {
      phone_.reset();
      other = desktop_;
    }
  }
  if (other) other->send_text("{\"type\":\"peer_left\"}");
}

void CamHub::relay(const std::shared_ptr<WsPeer>& src, const std::string& json) {
  std::shared_ptr<WsPeer> dest;
  {
    std::lock_guard<std::mutex> lock(mu_);
    dest = (src == desktop_) ? phone_ : desktop_;
  }
  if (!dest) {
    src->send_text("{\"type\":\"error\",\"message\":\"对方还没连上\"}");
    return;
  }
  dest->send_text(json);
}

void CamHub::broadcast_ready() {
  std::shared_ptr<WsPeer> a, b;
  {
    std::lock_guard<std::mutex> lock(mu_);
    a = desktop_;
    b = phone_;
  }
  if (!a || !b) return;
  a->send_text("{\"type\":\"ready\"}");
  b->send_text("{\"type\":\"ready\"}");
}

}  // namespace dustx
