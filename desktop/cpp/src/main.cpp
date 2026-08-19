#include "app.hpp"
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
  dustx::Server server;
  if (!server.start()) {
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
