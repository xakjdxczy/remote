#include "app.hpp"
#include "log.hpp"
#include "server.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <string>

#ifdef _WIN32
static void fail(const wchar_t* text) { MessageBoxW(nullptr, text, L"DustX", MB_OK | MB_ICONERROR); }
#else
#include <iostream>
static void fail(const char* text) { std::cerr << text << '\n'; }
#endif

static int dustx_main() {
  dustx::log_info("app", "尘埃X 启动");
  dustx::log_info("app", std::string("日志文件 ") + dustx::log_file_path());
  dustx::Server server;
  if (!server.start()) {
    dustx::log_error("app", "无法监听本机端口");
#ifdef _WIN32
    fail(L"无法监听本机端口，手机摄像头配对服务没有启动。");
#else
    fail("无法监听本机端口，手机摄像头配对服务没有启动。");
#endif
    return 1;
  }
  return dustx::run_native_app(server.port());
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return dustx_main(); }
#else
int main() { return dustx_main(); }
#endif
