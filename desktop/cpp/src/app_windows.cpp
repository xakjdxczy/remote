#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include "app.hpp"
#include "settings.hpp"
#include "update.hpp"
#include "util.hpp"
#include "vcam_install.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_web;
int g_port = 0;
int g_nav_tries = 0;
bool g_allow_close = false;
bool g_close_checking = false;

struct ExtraWin {
  HWND hwnd = nullptr;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> web;
};

std::vector<std::unique_ptr<ExtraWin>> g_extras;

void wire_web(ICoreWebView2* web);
void open_extra(const std::wstring& uri);
void navigate();

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
  web->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            const bool ok = is_app_url(uri);
            if (uri) CoTaskMemFree(uri);
            if (!ok && g_port > 0) {
              args->put_Cancel(TRUE);
              navigate();
            }
            return S_OK;
          })
          .Get(),
      nullptr);
  web->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL ok = TRUE;
            if (args) args->get_IsSuccess(&ok);
            if (ok) {
              g_nav_tries = 0;
              return S_OK;
            }
            if (g_nav_tries < 8) {
              g_nav_tries += 1;
              navigate();
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
  g_web->Navigate(buf);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SIZE:
      resize_web();
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
  return std::wstring(appdata) + L"\\DustX\\WebView2-ui";
}

}  // namespace

namespace dustx {

int run_native_app(int port) {
  g_port = port;
  if (!use_system_proxy()) {
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                            L"--proxy-server=direct:// --proxy-bypass-list=<-loopback>;127.0.0.1;localhost");
  } else {
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"");
  }
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

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
              MessageBoxW(g_hwnd, L"需要安装 Microsoft Edge WebView2 Runtime 才能打开窗口。", L"DustX", MB_OK);
              PostQuitMessage(1);
              return result;
            }
            g_env = env;
            env->CreateCoreWebView2Controller(
                g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                              if (FAILED(result2) || !controller) return result2;
                              g_controller = controller;
                              g_controller->get_CoreWebView2(&g_web);
                              resize_web();
                              wire_web(g_web.Get());
                              navigate();
                              return S_OK;
                            })
                            .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    MessageBoxW(g_hwnd, L"无法创建 WebView2。请安装 Edge WebView2 Runtime。", L"DustX", MB_OK);
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
