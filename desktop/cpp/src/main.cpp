#include "app.hpp"
#include "log.hpp"
#include "server.hpp"
#include "update.hpp"
#include "util.hpp"

#include <string>

static int dustx_main() {
  dustx::log_info("app", "尘埃X 启动");
  dustx::log_info("app", std::string("日志文件 ") + dustx::log_file_path());
  dustx::Server server;
  if (!server.start()) {
    dustx::log_error("app", "无法监听本机端口");
    dustx::alert_error("无法监听本机端口，界面和手机摄像头都用不了。\n\n日志：" + dustx::log_file_path());
    return 1;
  }
  if (!server.has_shell_ui()) {
    dustx::log_error("app", "缺少界面文件 " + server.web_dir() + "/shell.html");
    dustx::alert_error("界面文件缺失，窗口会是白屏。\n请重新下载安装尘埃X。\n\n目录：" + server.web_dir() +
                       "\n日志：" + dustx::log_file_path());
  }
  dustx::start_update_watcher();
  return dustx::run_native_app(server.port());
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return dustx_main(); }
#else
int main() { return dustx_main(); }
#endif
