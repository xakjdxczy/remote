#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ssh_host.hpp"

#include "log.hpp"
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

std::string win_err(DWORD code) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::string text = buf ? wide_to_utf8(buf) : std::to_string(code);
  if (buf) LocalFree(buf);
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
  return "err=" + std::to_string(code) + " " + text;
}

std::string acp_to_utf8(const std::string& s) {
  if (s.empty()) return {};
  int wlen = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (wlen <= 0) return s;
  std::wstring w(static_cast<size_t>(wlen), 0);
  MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), wlen);
  return wide_to_utf8(w);
}

void flush_lines(std::string& acc, const char* tag) {
  size_t pos = 0;
  while (true) {
    const size_t nl = acc.find('\n', pos);
    if (nl == std::string::npos) break;
    std::string line = acc.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) log_info(tag, acp_to_utf8(line));
    pos = nl + 1;
  }
  acc.erase(0, pos);
}

int run_hidden(const std::wstring& exe, const std::wstring& args, const char* tag, DWORD timeout_ms) {
  log_info(tag, "启动 " + wide_to_utf8(exe) + " " + wide_to_utf8(args));
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
    log_error(tag, std::string("CreatePipe 失败 ") + win_err(GetLastError()));
    return -1;
  }
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
    log_error(tag, std::string("CreateProcess 失败 ") + win_err(GetLastError()));
    CloseHandle(read_pipe);
    return -1;
  }
  log_info(tag, "进程已启动 pid=" + std::to_string(GetProcessId(pi.hProcess)));

  std::string acc;
  char chunk[1024];
  DWORD got = 0;
  const DWORD start = GetTickCount();
  const DWORD deadline = start + timeout_ms;
  DWORD last_beat = start;
  bool timed_out = false;
  for (;;) {
    const DWORD wait = WaitForSingleObject(pi.hProcess, 200);
    while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &got, nullptr) && got) {
      DWORD n = 0;
      if (!ReadFile(read_pipe, chunk, sizeof(chunk), &n, nullptr) || n == 0) break;
      acc.append(chunk, n);
      flush_lines(acc, tag);
    }
    const DWORD now = GetTickCount();
    if (now - last_beat >= 10000) {
      last_beat = now;
      log_info(tag, "仍在运行，已等待 " + std::to_string((now - start) / 1000) + " 秒");
    }
    if (wait == WAIT_OBJECT_0) break;
    if (now > deadline) {
      timed_out = true;
      log_error(tag, "超时，正在结束进程");
      TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  while (ReadFile(read_pipe, chunk, sizeof(chunk), &got, nullptr) && got) {
    acc.append(chunk, got);
    flush_lines(acc, tag);
  }
  if (!acc.empty()) log_info(tag, acp_to_utf8(acc));
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(read_pipe);
  log_info(tag, std::string("进程结束 code=") + std::to_string(code) + (timed_out ? " (超时)" : ""));
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
  log_info("ssh", "正在启动 sshd 服务");
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  if (!scm) {
    const std::string e = std::string("无法打开服务管理器 ") + win_err(GetLastError());
    log_error("ssh", e);
    if (err) *err = "无法打开服务管理器，请用管理员运行尘埃X。";
    return false;
  }
  SC_HANDLE svc = OpenServiceW(scm, L"sshd", SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
  if (!svc) {
    log_error("ssh", std::string("OpenService sshd 失败 ") + win_err(GetLastError()));
    CloseServiceHandle(scm);
    if (err) *err = "还没有 OpenSSH 服务。";
    return false;
  }
  ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr);
  SERVICE_STATUS st{};
  QueryServiceStatus(svc, &st);
  log_info("ssh", "sshd 当前状态=" + std::to_string(st.dwCurrentState));
  if (st.dwCurrentState != SERVICE_RUNNING) {
    if (!StartServiceW(svc, 0, nullptr)) {
      const DWORD e = GetLastError();
      if (e != ERROR_SERVICE_ALREADY_RUNNING) {
        log_error("ssh", std::string("StartService 失败 ") + win_err(e));
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
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
  log_info("ssh", std::string("sshd 启动结果 运行中=") + (st.dwCurrentState == SERVICE_RUNNING ? "是" : "否"));
  return st.dwCurrentState == SERVICE_RUNNING;
}

void allow_ssh_firewall() {
  log_info("ssh", "写入防火墙规则 DustX SSH / TCP 22");
  run_hidden(system_exe(L"netsh.exe"),
             L"advfirewall firewall add rule name=\"DustX SSH\" dir=in action=allow protocol=TCP localport=22", "ssh-fw",
             20000);
}

bool wait_port(int port, int tries) {
  for (int i = 0; i < tries; ++i) {
    if (port_open(port)) {
      log_info("ssh", "127.0.0.1:" + std::to_string(port) + " 已在监听");
      return true;
    }
    Sleep(250);
  }
  log_error("ssh", "等待 127.0.0.1:" + std::to_string(port) + " 超时");
  return false;
}

}  // namespace

SshHostStatus ssh_host_probe() {
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

SshHostStatus ssh_host_enable_work() {
  SshHostStatus s = ssh_host_probe();
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
    log_error("ssh", s.message);
    return s;
  }

  if (service_exists(L"sshd")) {
    log_info("ssh", "已有 sshd，跳过 DISM 安装");
    std::string err;
    if (!start_sshd(&err)) {
      s.ok = false;
      s.message = err;
      return s;
    }
  } else {
    log_info("ssh", "未找到 sshd，开始 DISM 安装 OpenSSH.Server");
    const int code = run_hidden(system_exe(L"dism.exe"),
                                L"/Online /Add-Capability /CapabilityName:OpenSSH.Server~~~~0.0.1.0 /NoRestart", "dism",
                                8 * 60 * 1000);
    const bool have = service_exists(L"sshd");
    log_info("ssh", std::string("DISM 结束 code=") + std::to_string(code) + " sshd=" + (have ? "有" : "无"));
    if (code != 0 && !have) {
      s.ok = false;
      s.message = "安装 OpenSSH 失败。完整原因见下方运行日志。";
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
    s.message = "OpenSSH 服务已启动，但 22 端口还没在听。请看下方日志。";
    return s;
  }
  s.ok = true;
  s.message = "本机 SSH 已开启。对方连上后，在对方电脑用这个用户名登录。";
  return s;
}

}  // namespace dustx
