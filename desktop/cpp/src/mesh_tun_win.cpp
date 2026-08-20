#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "mesh_tun.hpp"
#include "log.hpp"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace dustx {
namespace {

using WINTUN_ADAPTER_HANDLE = void*;
using WINTUN_SESSION_HANDLE = void*;
using WintunCreateAdapterFn = WINTUN_ADAPTER_HANDLE(__stdcall*)(const wchar_t*, const wchar_t*, const GUID*);
using WintunOpenAdapterFn = WINTUN_ADAPTER_HANDLE(__stdcall*)(const wchar_t*);
using WintunCloseAdapterFn = void(__stdcall*)(WINTUN_ADAPTER_HANDLE);
using WintunGetAdapterLuidFn = void(__stdcall*)(WINTUN_ADAPTER_HANDLE, NET_LUID*);
using WintunStartSessionFn = WINTUN_SESSION_HANDLE(__stdcall*)(WINTUN_ADAPTER_HANDLE, DWORD);
using WintunEndSessionFn = void(__stdcall*)(WINTUN_SESSION_HANDLE);
using WintunGetReadWaitEventFn = HANDLE(__stdcall*)(WINTUN_SESSION_HANDLE);
using WintunReceivePacketFn = unsigned char*(__stdcall*)(WINTUN_SESSION_HANDLE, DWORD*);
using WintunReleaseReceivePacketFn = void(__stdcall*)(WINTUN_SESSION_HANDLE, const unsigned char*);
using WintunAllocateSendPacketFn = unsigned char*(__stdcall*)(WINTUN_SESSION_HANDLE, DWORD);
using WintunSendPacketFn = void(__stdcall*)(WINTUN_SESSION_HANDLE, const unsigned char*);

struct WintunApi {
  HMODULE dll = nullptr;
  WintunCreateAdapterFn CreateAdapter = nullptr;
  WintunOpenAdapterFn OpenAdapter = nullptr;
  WintunCloseAdapterFn CloseAdapter = nullptr;
  WintunGetAdapterLuidFn GetAdapterLUID = nullptr;
  WintunStartSessionFn StartSession = nullptr;
  WintunEndSessionFn EndSession = nullptr;
  WintunGetReadWaitEventFn GetReadWaitEvent = nullptr;
  WintunReceivePacketFn ReceivePacket = nullptr;
  WintunReleaseReceivePacketFn ReleaseReceivePacket = nullptr;
  WintunAllocateSendPacketFn AllocateSendPacket = nullptr;
  WintunSendPacketFn SendPacket = nullptr;
};

WintunApi g_api;
std::mutex g_mu;
WINTUN_ADAPTER_HANDLE g_adapter = nullptr;
WINTUN_SESSION_HANDLE g_session = nullptr;
std::atomic<bool> g_run{false};
std::thread g_th;
TunSend g_send;
std::string g_dll_path;

std::string wide_path(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string o(static_cast<size_t>(n - 1), 0);
  WideCharToMultiByte(CP_UTF8, 0, w, -1, o.data(), n, nullptr, nullptr);
  return o;
}

bool file_exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring exe_dir_w() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring w(path);
  const auto slash = w.find_last_of(L"\\/");
  if (slash != std::wstring::npos) w.resize(slash);
  return w;
}

std::wstring find_wintun() {
  const std::wstring dir = exe_dir_w();
  const std::wstring cands[] = {dir + L"\\wintun.dll", L"wintun.dll"};
  for (const auto& p : cands) {
    if (file_exists(p)) return p;
  }
  return {};
}

bool is_admin() {
  BOOL admin = FALSE;
  PSID group = nullptr;
  SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
  if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
    CheckTokenMembership(nullptr, group, &admin);
    FreeSid(group);
  }
  return admin == TRUE;
}

bool load_wintun(std::string* reason) {
  if (g_api.dll) return true;
  const std::wstring path = find_wintun();
  if (path.empty()) {
    if (reason) *reason = "未找到 wintun.dll。请把官方 Wintun 放到 DustX.exe 同目录后再开虚拟网卡。";
    return false;
  }
  HMODULE dll = LoadLibraryW(path.c_str());
  if (!dll) {
    if (reason) *reason = "无法加载 wintun.dll。";
    return false;
  }
  auto sym = [&](const char* name) { return GetProcAddress(dll, name); };
  g_api.CreateAdapter = reinterpret_cast<WintunCreateAdapterFn>(sym("WintunCreateAdapter"));
  g_api.OpenAdapter = reinterpret_cast<WintunOpenAdapterFn>(sym("WintunOpenAdapter"));
  g_api.CloseAdapter = reinterpret_cast<WintunCloseAdapterFn>(sym("WintunCloseAdapter"));
  g_api.GetAdapterLUID = reinterpret_cast<WintunGetAdapterLuidFn>(sym("WintunGetAdapterLUID"));
  g_api.StartSession = reinterpret_cast<WintunStartSessionFn>(sym("WintunStartSession"));
  g_api.EndSession = reinterpret_cast<WintunEndSessionFn>(sym("WintunEndSession"));
  g_api.GetReadWaitEvent = reinterpret_cast<WintunGetReadWaitEventFn>(sym("WintunGetReadWaitEvent"));
  g_api.ReceivePacket = reinterpret_cast<WintunReceivePacketFn>(sym("WintunReceivePacket"));
  g_api.ReleaseReceivePacket = reinterpret_cast<WintunReleaseReceivePacketFn>(sym("WintunReleaseReceivePacket"));
  g_api.AllocateSendPacket = reinterpret_cast<WintunAllocateSendPacketFn>(sym("WintunAllocateSendPacket"));
  g_api.SendPacket = reinterpret_cast<WintunSendPacketFn>(sym("WintunSendPacket"));
  if (!g_api.CreateAdapter || !g_api.CloseAdapter || !g_api.StartSession || !g_api.ReceivePacket) {
    FreeLibrary(dll);
    g_api = {};
    if (reason) *reason = "wintun.dll 符号不完整。";
    return false;
  }
  g_api.dll = dll;
  g_dll_path = wide_path(path.c_str());
  return true;
}

bool parse_ipv4(const std::string& s, in_addr* out) {
  if (!out) return false;
  return inet_pton(AF_INET, s.c_str(), out) == 1;
}

bool assign_ipv4(NET_LUID luid, const std::string& local_ip, const std::string& peer_ip, std::string* err) {
  in_addr local{};
  in_addr peer{};
  if (!parse_ipv4(local_ip, &local) || !parse_ipv4(peer_ip, &peer)) {
    if (err) *err = "虚拟网卡地址必须是 IPv4，例如 100.64.0.1 / 100.64.0.2。";
    return false;
  }

  MIB_UNICASTIPADDRESS_ROW row;
  InitializeUnicastIpAddressEntry(&row);
  row.InterfaceLuid = luid;
  row.Address.Ipv4.sin_family = AF_INET;
  row.Address.Ipv4.sin_addr = local;
  row.OnLinkPrefixLength = 24;
  row.DadState = IpDadStatePreferred;
  DWORD st = CreateUnicastIpAddressEntry(&row);
  if (st != NO_ERROR && st != ERROR_OBJECT_ALREADY_EXISTS) {
    if (err) *err = "无法给虚拟网卡设置地址（需要管理员权限）。";
    return false;
  }

  MIB_IPFORWARD_ROW2 route;
  InitializeIpForwardEntry(&route);
  route.InterfaceLuid = luid;
  route.DestinationPrefix.Prefix.si_family = AF_INET;
  route.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
  route.DestinationPrefix.Prefix.Ipv4.sin_addr = peer;
  route.DestinationPrefix.PrefixLength = 32;
  route.NextHop.si_family = AF_INET;
  route.NextHop.Ipv4.sin_family = AF_INET;
  route.NextHop.Ipv4.sin_addr.s_addr = 0;
  route.Metric = 5;
  st = CreateIpForwardEntry2(&route);
  if (st != NO_ERROR && st != ERROR_OBJECT_ALREADY_EXISTS) {
    // Address is still usable on the same /24; route is best-effort.
  }
  return true;
}

void recv_loop() {
  while (g_run.load()) {
    WINTUN_SESSION_HANDLE session = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      session = g_session;
    }
    if (!session || !g_api.ReceivePacket) break;
    DWORD size = 0;
    unsigned char* pkt = g_api.ReceivePacket(session, &size);
    if (pkt) {
      if (size >= 20 && g_send) g_send(pkt, size);
      if (g_api.ReleaseReceivePacket) g_api.ReleaseReceivePacket(session, pkt);
      continue;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_NO_MORE_ITEMS) {
      HANDLE ev = g_api.GetReadWaitEvent ? g_api.GetReadWaitEvent(session) : nullptr;
      if (ev) WaitForSingleObject(ev, 250);
      else Sleep(10);
      continue;
    }
    break;
  }
}

}  // namespace

bool platform_tun_available(std::string* reason) {
  if (!load_wintun(reason)) return false;
  if (reason) {
    *reason = is_admin() ? "已找到 Wintun，可以开 100.x 虚拟网卡。"
                         : "已找到 Wintun。开启虚拟网卡需要以管理员运行尘埃X。";
  }
  return true;
}

bool platform_tun_start(const std::string& local_ip, const std::string& peer_ip, TunSend send, std::string* err) {
  platform_tun_stop();
  std::string reason;
  if (!load_wintun(&reason)) {
    log_error("tun", reason);
    if (err) *err = reason;
    return false;
  }
  if (!is_admin()) {
    log_error("tun", "当前进程没有管理员权限");
    if (err) *err = "开启虚拟网卡需要管理员权限（Wintun 装适配器）。应用层隧道不需要。";
    return false;
  }

  const std::string lip = local_ip.empty() ? "100.64.0.1" : local_ip;
  const std::string pip = peer_ip.empty() ? "100.64.0.2" : peer_ip;

  WINTUN_ADAPTER_HANDLE adapter = g_api.CreateAdapter(L"尘埃X", L"DustX", nullptr);
  if (!adapter && g_api.OpenAdapter) adapter = g_api.OpenAdapter(L"尘埃X");
  if (!adapter) {
    log_error("tun", "无法创建或打开 Wintun 适配器");
    if (err) *err = "无法创建 Wintun 适配器。请用管理员运行，或检查 wintun.dll。";
    return false;
  }
  log_info("tun", "已创建 Wintun 适配器 " + lip + " 对端 " + pip);

  NET_LUID luid{};
  if (g_api.GetAdapterLUID) g_api.GetAdapterLUID(adapter, &luid);
  if (!assign_ipv4(luid, lip, pip, err)) {
    g_api.CloseAdapter(adapter);
    return false;
  }

  WINTUN_SESSION_HANDLE session = g_api.StartSession(adapter, 0x400000);
  if (!session) {
    g_api.CloseAdapter(adapter);
    if (err) *err = "Wintun 会话启动失败。";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_adapter = adapter;
    g_session = session;
    g_send = std::move(send);
    g_run = true;
  }
  g_th = std::thread(recv_loop);
  return true;
}

void platform_tun_stop() {
  g_run = false;
  WINTUN_SESSION_HANDLE session = nullptr;
  WINTUN_ADAPTER_HANDLE adapter = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    session = g_session;
    adapter = g_adapter;
    g_session = nullptr;
    g_adapter = nullptr;
    g_send = nullptr;
  }
  if (session && g_api.EndSession) g_api.EndSession(session);
  if (g_th.joinable()) g_th.join();
  if (adapter && g_api.CloseAdapter) g_api.CloseAdapter(adapter);
}

bool platform_tun_inject(const uint8_t* pkt, size_t n) {
  if (!pkt || n < 20 || n > 0xFFFF) return false;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_session || !g_api.AllocateSendPacket || !g_api.SendPacket) return false;
  unsigned char* dest = g_api.AllocateSendPacket(g_session, static_cast<DWORD>(n));
  if (!dest) return false;
  memcpy(dest, pkt, n);
  g_api.SendPacket(g_session, dest);
  return true;
}

}  // namespace dustx
