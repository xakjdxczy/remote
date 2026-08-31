#include "app.hpp"
#include "log.hpp"
#include "server.hpp"
#include "update.hpp"
#include "util.hpp"
#include "version.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "crash_win.hpp"
#endif

#include <string>

static int dustx_main() {
  dustx::log_info("app", std::string("尘埃 启动 ") + dustx::kAppVersion);
  dustx::log_info("app", std::string("日志文件 ") + dustx::log_file_path());
#ifdef _WIN32
  dustx::log_info("app", "pid=" + std::to_string(GetCurrentProcessId()) + " 崩溃目录 " + dustx::crash_dir());
#endif
  dustx::Server server;
  if (!server.start()) {
    dustx::log_error("app", "无法监听本机端口");
    dustx::alert_error("无法监听本机端口，界面和手机摄像头都用不了。\n\n日志：" + dustx::log_file_path());
    return 1;
  }
  if (!server.has_shell_ui()) {
    dustx::log_error("app", "缺少界面文件 " + server.web_dir() + "/shell.html");
    dustx::alert_error("界面文件缺失，窗口会是白屏。\n请重新下载安装尘埃。\n\n目录：" + server.web_dir() +
                       "\n日志：" + dustx::log_file_path());
  }
  dustx::start_update_watcher();
  return dustx::run_native_app(server.port());
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  dustx::install_crash_handler();
  using SetDpiFn = BOOL(WINAPI*)(void*);
  auto set_dpi = reinterpret_cast<SetDpiFn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (!set_dpi || !set_dpi(reinterpret_cast<void*>(static_cast<LONG_PTR>(-4)))) {
    SetProcessDPIAware();
  }
  return dustx_main();
}
#else
int main() { return dustx_main(); }
#endif
