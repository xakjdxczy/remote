#pragma once

#include <functional>
#include <string>

namespace dustx {

// Default is off: DustX does not follow the OS / Clash system proxy.
bool use_system_proxy();
void set_use_system_proxy(bool on);
std::string settings_json();
void apply_settings_body(const std::string& body);
void set_proxy_apply(std::function<void(bool use_system)> fn);

}  // namespace dustx
