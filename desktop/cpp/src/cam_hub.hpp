#pragma once

#include <memory>
#include <mutex>
#include <string>

namespace dustx {

class WsPeer {
 public:
  virtual ~WsPeer() = default;
  virtual bool send_text(const std::string& json) = 0;
};

class CamHub {
 public:
  CamHub();
  std::string token() const;
  std::string rotate_token();
  std::string info_json(int port) const;

  bool attach(const std::shared_ptr<WsPeer>& peer, const std::string& role, const std::string& token);
  void detach(const std::shared_ptr<WsPeer>& peer);
  void relay(const std::shared_ptr<WsPeer>& src, const std::string& json);

 private:
  void broadcast_ready();

  mutable std::mutex mu_;
  std::string token_;
  std::shared_ptr<WsPeer> desktop_;
  std::shared_ptr<WsPeer> phone_;
};

}  // namespace dustx
