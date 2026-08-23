#pragma once

#include <functional>
#include <string>

namespace dustx {

void set_update_quit(std::function<void()> fn);
void start_update_watcher();
std::string check_update_json();
std::string update_status_json();
std::string apply_update_json(bool force);

}  // namespace dustx
