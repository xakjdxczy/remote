#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include "app.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "update.hpp"
#include "util.hpp"
#include "vcam_install.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_DUSTX_NAV = WM_APP + 21;

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_web;
int g_port = 0;
int g_nav_tries = 0;
bool g_allow_close = false;
bool g_close_checking = false;
bool g_nav_alerted = false;

struct ExtraWin {
  HWND hwnd = nullptr;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> web;
};

std::vector<std::unique_ptr<ExtraWin>> g_extras;

void wire_web(ICoreWebView2* web);
void open_extra(const std::wstring& uri);
void navigate();
void request_navigate();

std::wstring webview_browser_args() {
  // Do not pass --enable-features here. Experimental WebRTC/WGC flags have
  // left WebView2 permanently blank on dual-GPU Windows after 2026.8.26.2.
  if (!dustx::use_system_proxy()) {
    return L"--proxy-server=direct:// --proxy-bypass-list=<-loopback>;127.0.0.1;localhost";
  }
  return L"";
}

void request_navigate() {
  if (g_hwnd) PostMessageW(g_hwnd, WM_DUSTX_NAV, 0, 0);
}

std::string hr_hex(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
  return buf;
}

void log_wide(const char* msg, const wchar_t* value) {
  dustx::log_info("webview", std::string(msg) + (value ? dustx::wide_to_utf8(value) : "(null)"));
}

void allow_media_permissions(ICoreWebView2* web) {
  web->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
            if (args) args->get_PermissionKind(&kind);
            if (kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA ||
                kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE ||
                kind == COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY) {
              args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
            }
            return S_OK;
          })
          .Get(),
      nullptr);
}

LRESULT CALLBACK extra_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  ExtraWin* extra = reinterpret_cast<ExtraWin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_SIZE:
      if (extra && extra->controller) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        extra->controller->put_Bounds(rc);
      }
      return 0;
    case WM_DESTROY: {
      ExtraWin* doomed = extra;
      if (doomed) {
        doomed->web.Reset();
        doomed->controller.Reset();
        doomed->hwnd = nullptr;
        g_extras.erase(std::remove_if(g_extras.begin(), g_extras.end(),
                                      [&](const std::unique_ptr<ExtraWin>& p) { return p.get() == doomed; }),
                       g_extras.end());
      }
      return 0;
    }
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

void open_extra(const std::wstring& uri) {
  if (!g_env) return;
  auto extra = std::make_unique<ExtraWin>();
  ExtraWin* raw = extra.get();
  raw->hwnd = CreateWindowExW(0, L"DustXExtraWnd", L"尘埃X", WS_OVERLAPPEDWINDOW, 160, 80, 1100, 740, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  if (!raw->hwnd) return;
  SetWindowLongPtrW(raw->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(raw));
  ShowWindow(raw->hwnd, SW_SHOW);
  std::wstring target = uri;
  g_env->CreateCoreWebView2Controller(
      raw->hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                     [raw, target](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                       if (FAILED(result) || !controller) return result;
                       raw->controller = controller;
                       raw->controller->get_CoreWebView2(&raw->web);
                       RECT rc{};
                       GetClientRect(raw->hwnd, &rc);
                       raw->controller->put_Bounds(rc);
                       if (raw->web) {
                         wire_web(raw->web.Get());
                         raw->web->Navigate(target.c_str());
                       }
                       return S_OK;
                     })
                     .Get());
  g_extras.push_back(std::move(extra));
}

bool is_app_url(const wchar_t* uri) {
  if (!uri || g_port <= 0) return false;
  wchar_t need[64];
  swprintf(need, 64, L"http://127.0.0.1:%d", g_port);
  return wcsstr(uri, need) != nullptr;
}

void wire_web(ICoreWebView2* web) {
  if (!web) return;
  allow_media_permissions(web);
  web->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            const bool ok = is_app_url(uri);
            if (!ok && g_port > 0) {
              log_wide("拦截跳转 ", uri);
              args->put_Cancel(TRUE);
              request_navigate();
            }
            if (uri) CoTaskMemFree(uri);
            return S_OK;
          })
          .Get(),
      nullptr);
  web->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL ok = TRUE;
            COREWEBVIEW2_WEB_ERROR_STATUS err = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
            if (args) {
              args->get_IsSuccess(&ok);
              args->get_WebErrorStatus(&err);
            }
            LPWSTR src = nullptr;
            if (g_web) g_web->get_Source(&src);
            dustx::log_info("webview", std::string(ok ? "页面打开成功 " : "页面打开失败 ") +
                                           (src ? dustx::wide_to_utf8(src) : "") + " err=" +
                                           std::to_string(static_cast<int>(err)));
            if (src) CoTaskMemFree(src);
            if (ok) {
              g_nav_tries = 0;
              return S_OK;
            }
            if (g_nav_tries < 8) {
              g_nav_tries += 1;
              dustx::log_warn("webview", "重试打开界面 " + std::to_string(g_nav_tries) + "/8");
              request_navigate();
            } else if (!g_nav_alerted) {
              g_nav_alerted = true;
              dustx::log_error("webview", "界面多次打开失败，窗口会是白屏");
              dustx::alert_error(
                  "无法打开本机界面，窗口是白屏。\n请删除 %LOCALAPPDATA%\\DustX\\WebView2-v3 后再开，"
                  "或重新下载安装。\n\n日志：" +
                  dustx::log_file_path());
            }
            return S_OK;
          })
          .Get(),
      nullptr);
  web->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            if (uri) {
              open_extra(uri);
              CoTaskMemFree(uri);
            }
            return S_OK;
          })
          .Get(),
      nullptr);
}

bool parse_close_check(const wchar_t* json, bool* confirm, std::wstring* text) {
  if (!json || !confirm) return false;
  *confirm = wcsstr(json, L"\"confirm\":true") != nullptr || wcsstr(json, L"\"confirm\": true") != nullptr;
  if (text) {
    const wchar_t* key = wcsstr(json, L"\"text\":\"");
    if (key) {
      key += 8;
      const wchar_t* end = wcschr(key, L'"');
      if (end && end > key) text->assign(key, end);
    }
  }
  return true;
}

void ask_close_then_destroy() {
  if (g_allow_close || !g_web) {
    g_allow_close = true;
    DestroyWindow(g_hwnd);
    return;
  }
  if (g_close_checking) return;
  g_close_checking = true;
  g_web->ExecuteScript(
      L"(function(){try{return window.dustxCloseCheck?window.dustxCloseCheck():{confirm:false};}catch(e){return {confirm:false};}})()",
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>([](HRESULT, LPCWSTR result) -> HRESULT {
        g_close_checking = false;
        bool confirm = false;
        std::wstring what;
        parse_close_check(result, &confirm, &what);
        if (confirm) {
          std::wstring body = L"当前还有连接";
          if (!what.empty()) body += L"：" + what;
          body += L"。\n关闭后这些连接会断开。\n\n确定关闭尘埃X？";
          if (MessageBoxW(g_hwnd, body.c_str(), L"尘埃X", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
            return S_OK;
          }
        }
        g_allow_close = true;
        if (g_hwnd) DestroyWindow(g_hwnd);
        return S_OK;
      }).Get());
}

void resize_web() {
  if (!g_controller || !g_hwnd) return;
  RECT rc{};
  GetClientRect(g_hwnd, &rc);
  g_controller->put_Bounds(rc);
}

void navigate() {
  if (!g_web) return;
  wchar_t buf[128];
  swprintf(buf, 128, L"http://127.0.0.1:%d/", g_port);
  log_wide("打开 ", buf);
  g_web->Navigate(buf);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SIZE:
      resize_web();
      return 0;
    case WM_DUSTX_NAV:
      navigate();
      return 0;
    case WM_TIMER:
      if (wparam == 1) {
        KillTimer(hwnd, 1);
        navigate();
      }
      return 0;
    case WM_CLOSE:
      if (g_allow_close) {
        DestroyWindow(hwnd);
        return 0;
      }
      ask_close_then_destroy();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

std::wstring user_data_dir() {
  wchar_t appdata[MAX_PATH];
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) == 0) return L".\\DustXWebView";
  return std::wstring(appdata) + L"\\DustX\\WebView2-v3";
}

}  // namespace

namespace dustx {

int run_native_app(int port) {
  g_port = port;
  SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", webview_browser_args().c_str());
  set_update_quit([] {
    g_allow_close = true;
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  });
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"DustXWnd";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);

  WNDCLASSW extra_wc = wc;
  extra_wc.lpfnWndProc = extra_proc;
  extra_wc.lpszClassName = L"DustXExtraWnd";
  RegisterClassW(&extra_wc);

  g_hwnd = CreateWindowExW(0, L"DustXWnd", L"尘埃X", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
                           nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);
  std::string unused;
  install_vcam(&unused);
  install_vmic(&unused);

  std::wstring data = user_data_dir();
  CreateDirectoryW((std::wstring(data.begin(), data.begin() + data.find_last_of(L'\\'))).c_str(), nullptr);
  CreateDirectoryW(data.c_str(), nullptr);
  SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", data.c_str());
  dustx::log_info("webview", "用户目录 " + dustx::wide_to_utf8(data));
  dustx::log_info("webview", "启动参数 " + dustx::wide_to_utf8(webview_browser_args()));

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
              dustx::log_error("webview", "创建 WebView2 环境失败 " + hr_hex(result));
              dustx::alert_error("无法创建界面内核（WebView2）。\n请安装 Microsoft Edge WebView2 Runtime 后再开。\n错误：" +
                                 hr_hex(result) + "\n\n日志：" + dustx::log_file_path());
              PostQuitMessage(1);
              return result;
            }
            dustx::log_info("webview", "WebView2 环境已创建");
            g_env = env;
            env->CreateCoreWebView2Controller(
                g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                              if (FAILED(result2) || !controller) {
                                dustx::log_error("webview", "创建 WebView2 控件失败 " + hr_hex(result2));
                                dustx::alert_error("无法创建界面窗口（WebView2 控件失败）。\n错误：" + hr_hex(result2) +
                                                   "\n\n日志：" + dustx::log_file_path());
                                PostQuitMessage(1);
                                return result2;
                              }
                              dustx::log_info("webview", "WebView2 控件已创建");
                              g_controller = controller;
                              g_controller->get_CoreWebView2(&g_web);
                              resize_web();
                              wire_web(g_web.Get());
                              navigate();
                              if (g_hwnd) SetTimer(g_hwnd, 1, 400, nullptr);
                              return S_OK;
                            })
                            .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    dustx::log_error("webview", "启动 WebView2 失败 " + hr_hex(hr));
    dustx::alert_error("无法启动界面内核（WebView2）。\n请安装 Microsoft Edge WebView2 Runtime。\n错误：" + hr_hex(hr) +
                       "\n\n日志：" + dustx::log_file_path());
    if (SUCCEEDED(co)) CoUninitialize();
    return 1;
  }

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g_web.Reset();
  g_controller.Reset();
  if (SUCCEEDED(co)) CoUninitialize();
  return static_cast<int>(msg.wParam);
}

}  // namespace dustx
