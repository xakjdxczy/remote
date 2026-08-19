#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include "app.hpp"
#include "util.hpp"
#include "vcam_install.hpp"

#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

HWND g_hwnd = nullptr;
HWND g_remote_btn = nullptr;
HWND g_cam_btn = nullptr;
HWND g_mesh_btn = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_web;
int g_port = 0;
int g_tab = 2;

constexpr int kBar = 48;
constexpr int kIdRemote = 1001;
constexpr int kIdCam = 1002;
constexpr int kIdMesh = 1003;

void resize_web() {
  if (!g_controller || !g_hwnd) return;
  RECT rc{};
  GetClientRect(g_hwnd, &rc);
  rc.top += kBar;
  g_controller->put_Bounds(rc);
}

void navigate() {
  if (!g_web) return;
  std::wstring url;
  if (g_tab == 2) {
    wchar_t buf[128];
    swprintf(buf, 128, L"http://127.0.0.1:%d/cam.html", g_port);
    url = buf;
  } else if (g_tab == 3) {
    wchar_t buf[128];
    swprintf(buf, 128, L"http://127.0.0.1:%d/mesh.html", g_port);
    url = buf;
  } else {
    url = dustx::utf8_to_wide(dustx::remote_console_url());
  }
  g_web->Navigate(url.c_str());
}

void invalidate_tabs() {
  if (g_remote_btn) InvalidateRect(g_remote_btn, nullptr, TRUE);
  if (g_cam_btn) InvalidateRect(g_cam_btn, nullptr, TRUE);
  if (g_mesh_btn) InvalidateRect(g_mesh_btn, nullptr, TRUE);
}

void layout_buttons() {
  if (!g_hwnd) return;
  MoveWindow(g_remote_btn, 12, 8, 120, 32, TRUE);
  MoveWindow(g_cam_btn, 140, 8, 120, 32, TRUE);
  MoveWindow(g_mesh_btn, 268, 8, 120, 32, TRUE);
}

void paint_bar(HDC hdc, RECT client) {
  RECT bar = client;
  bar.bottom = kBar;
  HBRUSH brush = CreateSolidBrush(RGB(15, 27, 45));
  FillRect(hdc, &bar, brush);
  DeleteObject(brush);
}

void draw_tab(const DRAWITEMSTRUCT* di, bool selected) {
  RECT rc = di->rcItem;
  COLORREF bg = selected ? RGB(47, 128, 237) : RGB(52, 68, 92);
  HBRUSH brush = CreateSolidBrush(bg);
  HPEN pen = CreatePen(PS_SOLID, 1, bg);
  HGDIOBJ old_brush = SelectObject(di->hDC, brush);
  HGDIOBJ old_pen = SelectObject(di->hDC, pen);
  RoundRect(di->hDC, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
  SelectObject(di->hDC, old_brush);
  SelectObject(di->hDC, old_pen);
  DeleteObject(brush);
  DeleteObject(pen);

  wchar_t text[64]{};
  GetWindowTextW(di->hwndItem, text, 64);
  SetBkMode(di->hDC, TRANSPARENT);
  SetTextColor(di->hDC, RGB(255, 255, 255));
  DrawTextW(di->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SIZE:
      layout_buttons();
      resize_web();
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == kIdRemote) {
        g_tab = 1;
        navigate();
        invalidate_tabs();
      } else if (LOWORD(wparam) == kIdCam) {
        g_tab = 2;
        navigate();
        invalidate_tabs();
      } else if (LOWORD(wparam) == kIdMesh) {
        g_tab = 3;
        navigate();
        invalidate_tabs();
      }
      return 0;
    case WM_DRAWITEM: {
      const auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (di->CtlID == kIdRemote || di->CtlID == kIdCam || di->CtlID == kIdMesh) {
        int tab = di->CtlID == kIdRemote ? 1 : di->CtlID == kIdCam ? 2 : 3;
        draw_tab(di, tab == g_tab);
        return TRUE;
      }
      break;
    }
    case WM_ERASEBKGND: {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      paint_bar(reinterpret_cast<HDC>(wparam), rc);
      return 1;
    }
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
  return std::wstring(appdata) + L"\\DustX\\WebView2";
}

}  // namespace

namespace dustx {

int run_native_app(int port) {
  g_port = port;
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"DustXWnd";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);

  g_hwnd = CreateWindowExW(0, L"DustXWnd", L"尘埃X", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780,
                           nullptr, nullptr, wc.hInstance, nullptr);
  g_remote_btn = CreateWindowW(L"BUTTON", L"远程控制", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 12, 8, 120, 32, g_hwnd,
                               reinterpret_cast<HMENU>(kIdRemote), wc.hInstance, nullptr);
  g_cam_btn = CreateWindowW(L"BUTTON", L"手机摄像头", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 140, 8, 120, 32, g_hwnd,
                            reinterpret_cast<HMENU>(kIdCam), wc.hInstance, nullptr);
  g_mesh_btn = CreateWindowW(L"BUTTON", L"跨网互访", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 268, 8, 120, 32, g_hwnd,
                             reinterpret_cast<HMENU>(kIdMesh), wc.hInstance, nullptr);
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
            env->CreateCoreWebView2Controller(
                g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                              if (FAILED(result2) || !controller) return result2;
                              g_controller = controller;
                              g_controller->get_CoreWebView2(&g_web);
                              resize_web();
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
