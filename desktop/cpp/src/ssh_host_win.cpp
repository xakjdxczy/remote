#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ssh_host.hpp"

#include "net.hpp"
#include "util.hpp"

#include <windows.h>
#include <winsvc.h>

#include <string>
#include <vector>

namespace dustx {
namespace {

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

std::string current_user() {
  wchar_t name[256];
  DWORD n = 256;
  if (!GetUserNameW(name, &n)) return {};
  return wide_to_utf8(name);
}

bool port_open(int port) {
  const int fd = connect_loopback(port);
  if (fd < 0) return false;
  close_fd(fd);
  return true;
}

std::wstring system_exe(const wchar_t* name) {
  wchar_t dir[MAX_PATH];
  UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return name;
  return std::wstring(dir) + L"\\" + name;
}

int run_hidden(const std::wstring& exe, const std::wstring& args, std::string* out, DWORD timeout_ms) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return -1;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(write_pipe);
  if (!ok) {
    CloseHandle(read_pipe);
    return -1;
  }

  std::string text;
  char chunk[512];
  DWORD got = 0;
  const DWORD deadline = GetTickCount() + timeout_ms;
  for (;;) {
    DWORD wait = WaitForSingleObject(pi.hProcess, 200);
    while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &got, nullptr) && got) {
      DWORD n = 0;
      if (!ReadFile(read_pipe, chunk, sizeof(chunk), &n, nullptr) || n == 0) break;
      text.append(chunk, n);
    }
    if (wait == WAIT_OBJECT_0) break;
    if (GetTickCount() > deadline) {
      TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  while (ReadFile(read_pipe, chunk, sizeof(chunk), &got, nullptr) && got) text.append(chunk, got);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(read_pipe);
  if (out) *out = text;
  return static_cast<int>(code);
}

bool service_exists(const wchar_t* name) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) return false;
  SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
  const bool ok = svc != nullptr;
  if (svc) CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return ok;
}

bool service_running(const wchar_t* name) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) return false;
  SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
  if (!svc) {
    CloseServiceHandle(scm);
    return false;
  }
  SERVICE_STATUS st{};
  const bool ok = QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return ok;
}

bool start_sshd(std::string* err) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  if (!scm) {
    if (err) *err = "无法打开服务管理器，请用管理员运行尘埃X。";
    return false;
  }
  SC_HANDLE svc = OpenServiceW(scm, L"sshd", SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
  if (!svc) {
    CloseServiceHandle(scm);
    if (err) *err = "还没有 OpenSSH 服务。";
    return false;
  }
  ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr);
  SERVICE_STATUS st{};
  QueryServiceStatus(svc, &st);
  if (st.dwCurrentState != SERVICE_RUNNING) {
    if (!StartServiceW(svc, 0, nullptr)) {
      const DWORD e = GetLastError();
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      if (e != ERROR_SERVICE_ALREADY_RUNNING) {
        if (err) *err = "无法启动 OpenSSH 服务。";
        return false;
      }
    }
  }
  for (int i = 0; i < 40; ++i) {
    QueryServiceStatus(svc, &st);
    if (st.dwCurrentState == SERVICE_RUNNING) break;
    Sleep(250);
  }
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return st.dwCurrentState == SERVICE_RUNNING;
}

void allow_ssh_firewall() {
  std::string out;
  run_hidden(system_exe(L"netsh.exe"),
             L"advfirewall firewall add rule name=\"DustX SSH\" dir=in action=allow protocol=TCP localport=22", &out,
             20000);
}

bool wait_port(int port, int tries) {
  for (int i = 0; i < tries; ++i) {
    if (port_open(port)) return true;
    Sleep(250);
  }
  return false;
}

std::string tail_text(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  return s.substr(s.size() - n);
}

}  // namespace

SshHostStatus ssh_host_status() {
  SshHostStatus s;
  s.os = "windows";
  s.username = current_user();
  s.can_enable = true;
  s.need_admin = !is_admin();
  s.ready = port_open(22);
  if (s.ready) {
    s.message = "本机 SSH 已开启。对方用尘埃X连上后，在对方电脑执行下面的命令，用户名就是现在这个 Windows 账户。";
  } else if (s.need_admin) {
    s.message = "这台电脑还不能被 SSH 登录。请右键尘埃X，选择「以管理员身份运行」，再点「开启本机 SSH」。";
  } else if (service_exists(L"sshd") && !service_running(L"sshd")) {
    s.message = "已安装 OpenSSH，但服务没开。点「开启本机 SSH」即可启动。";
  } else {
    s.message = "这台电脑还不能被 SSH 登录。点「开启本机 SSH」，尘埃X 会安装并启动 Windows 自带的 OpenSSH，不用命令行。";
  }
  return s;
}

SshHostStatus ssh_host_enable() {
  SshHostStatus s = ssh_host_status();
  if (s.ready) {
    allow_ssh_firewall();
    s.ok = true;
    s.message = "本机 SSH 已经可用。";
    return s;
  }
  if (!is_admin()) {
    s.ok = false;
    s.need_admin = true;
    s.message = "开启 SSH 需要管理员权限。请先关掉尘埃X，再右键选择「以管理员身份运行」。";
    return s;
  }

  if (service_exists(L"sshd")) {
    std::string err;
    if (!start_sshd(&err)) {
      s.ok = false;
      s.message = err;
      return s;
    }
  } else {
    std::string out;
    const int code = run_hidden(system_exe(L"dism.exe"),
                                L"/Online /Add-Capability /CapabilityName:OpenSSH.Server~~~~0.0.1.0 /NoRestart", &out,
                                15 * 60 * 1000);
    if (code != 0 && !service_exists(L"sshd")) {
      s.ok = false;
      s.message = "安装 OpenSSH 失败。请确认 Windows 更新可用后再试。";
      const std::string extra = tail_text(out, 240);
      if (!extra.empty()) s.message += extra;
      return s;
    }
    std::string err;
    if (!start_sshd(&err)) {
      s.ok = false;
      s.message = err.empty() ? "OpenSSH 已安装，但没能启动服务。" : err;
      return s;
    }
  }

  allow_ssh_firewall();
  s.ready = wait_port(22, 40);
  s.need_admin = false;
  s.username = current_user();
  if (!s.ready) {
    s.ok = false;
    s.message = "OpenSSH 服务已启动，但 22 端口还没在听。等几秒后重新打开跨网互访页再看。";
    return s;
  }
  s.ok = true;
  s.message = "本机 SSH 已开启。对方连上后，在对方电脑用这个用户名登录。";
  return s;
}

}  // namespace dustx
