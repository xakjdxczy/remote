#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace dustx {

using TunSend = std::function<void(const uint8_t* pkt, size_t n)>;

bool platform_tun_available(std::string* reason);
bool platform_tun_start(const std::string& local_ip, const std::string& peer_ip, TunSend send, std::string* err);
void platform_tun_stop();
bool platform_tun_inject(const uint8_t* pkt, size_t n);

}  // namespace dustx
