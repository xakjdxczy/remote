#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ssh_host.hpp"

#include "log.hpp"
#include "net.hpp"
#include "util.hpp"

#include <windows.h>
#include <winsvc.h>

#include <cstdint>
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

std::wstring module_dir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring w(path);
  const auto slash = w.find_last_of(L"\\/");
  if (slash != std::wstring::npos) w.resize(slash);
  return w;
}

std::wstring bundled_openssh_dir() { return module_dir() + L"\\openssh"; }

std::wstring bundled_sshd_exe() { return bundled_openssh_dir() + L"\\sshd.exe"; }

std::wstring powershell_exe() { return system_exe(L"WindowsPowerShell\\v1.0\\powershell.exe"); }

int run_hidden(const std::wstring& exe, const std::wstring& args, const char* tag, DWORD timeout_ms,
               const wchar_t* cwd = nullptr);
bool service_exists(const wchar_t* name);

bool install_bundled_openssh(std::string* err) {
  const std::wstring dir = bundled_openssh_dir();
  const std::wstring ps1 = dir + L"\\install-sshd.ps1";
  if (GetFileAttributesW(ps1.c_str()) == INVALID_FILE_ATTRIBUTES) {
    log_error("ssh", "安装包里没有 openssh\\install-sshd.ps1，请重新下载官网 Windows 程序");
    if (err) *err = "当前尘埃X 安装包不完整，请到官网重新下载 Windows 桌面程序。";
    return false;
  }
  log_info("ssh", "调用 install-sshd.ps1 -Confirm:$false（避免隐藏窗口下确认提示失败）");
  // install-sshd.ps1 is CmdletBinding ConfirmImpact=High. -NonInteractive without
  // -Confirm:$false aborts: "Read and Prompt functionality is not available."
  const std::wstring args =
      L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + ps1 + L"\" -Confirm:$false";
  const int code = run_hidden(powershell_exe(), args, "openssh", 120000, dir.c_str());
  if (code != 0 && !service_exists(L"sshd")) {
    log_error("ssh", "install-sshd.ps1 退出码=" + std::to_string(code));
    if (err) *err = "安装自带 OpenSSH 失败，原因见运行日志（openssh）。";
    return false;
  }
  if (code != 0) {
    log_info("ssh", "install-sshd.ps1 退出码=" + std::to_string(code) + "，但 sshd 服务已登记，继续启动");
  }
  return true;
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

int run_hidden(const std::wstring& exe, const std::wstring& args, const char* tag, DWORD timeout_ms,
               const wchar_t* cwd) {
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
  const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd, &si, &pi);
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

std::wstring expand_env(const std::wstring& s) {
  if (s.empty()) return s;
  DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
  if (!n) return s;
  std::wstring out(n, 0);
  ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
  while (!out.empty() && out.back() == L'\0') out.pop_back();
  return out;
}

std::wstring exe_from_image_path(const std::wstring& image) {
  std::wstring s = image;
  while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
  if (!s.empty() && s.front() == L'"') {
    const auto end = s.find(L'"', 1);
    if (end != std::wstring::npos) s = s.substr(1, end - 1);
  } else {
    const auto sp = s.find(L' ');
    if (sp != std::wstring::npos) s.resize(sp);
  }
  return expand_env(s);
}

bool file_exists_w(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool paths_same(const std::wstring& a, const std::wstring& b);
std::wstring service_image_path(const wchar_t* name);

const char* service_state_name(DWORD state) {
  switch (state) {
    case SERVICE_STOPPED:
      return "已停止";
    case SERVICE_START_PENDING:
      return "正在启动";
    case SERVICE_STOP_PENDING:
      return "正在停止";
    case SERVICE_RUNNING:
      return "运行中";
    case SERVICE_CONTINUE_PENDING:
      return "正在继续";
    case SERVICE_PAUSE_PENDING:
      return "正在暂停";
    case SERVICE_PAUSED:
      return "已暂停";
    default:
      return "未知";
  }
}

void log_sshd_snapshot(const char* why) {
  const std::wstring bundled = bundled_sshd_exe();
  const bool bundled_ok = file_exists_w(bundled);
  const bool has_svc = service_exists(L"sshd");
  const std::wstring image = service_image_path(L"sshd");
  const std::wstring exe = exe_from_image_path(image);
  const bool exe_ok = file_exists_w(exe);
  log_info("ssh", std::string("状态快照（") + why + "）");
  log_info("ssh", std::string("  管理员=") + (is_admin() ? "是" : "否") +
                       "，本机 22 端口=" + (port_open(22) ? "在听" : "未听"));
  log_info("ssh", "  当前尘埃X 的 sshd.exe=" + wide_to_utf8(bundled) + (bundled_ok ? "（文件在）" : "（文件不在）"));
  if (!has_svc) {
    log_info("ssh", "  系统服务 sshd=未登记");
  } else {
    log_info("ssh", "  系统服务 sshd=已登记" + std::string(service_running(L"sshd") ? "，正在运行" : "，未运行"));
    log_info("ssh", "  服务登记路径=" + (image.empty() ? std::string("（空）") : wide_to_utf8(image)));
    log_info("ssh", "  登记的 exe=" + (exe.empty() ? std::string("（空）") : wide_to_utf8(exe)) +
                         (exe_ok ? "（文件在）" : "（文件不在）"));
    if (bundled_ok && exe_ok && !paths_same(exe, bundled)) {
      log_info("ssh", "  登记路径不是当前尘埃X 目录，开启时会按当前目录重新登记");
    }
  }
  log_info("ssh", "  说明：尘埃X 用系统服务 API 开关 sshd。若在 PowerShell 里手工查/删服务，请用 sc.exe 或 Get-Service，不要用 sc（那是 Set-Content 写文件，成功时没有任何输出）。");
}

bool paths_same(const std::wstring& a, const std::wstring& b) {
  wchar_t fa[MAX_PATH];
  wchar_t fb[MAX_PATH];
  if (!GetFullPathNameW(a.c_str(), MAX_PATH, fa, nullptr)) return false;
  if (!GetFullPathNameW(b.c_str(), MAX_PATH, fb, nullptr)) return false;
  return _wcsicmp(fa, fb) == 0;
}

std::wstring service_image_path(const wchar_t* name) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) return {};
  SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_CONFIG);
  if (!svc) {
    CloseServiceHandle(scm);
    return {};
  }
  DWORD need = 0;
  QueryServiceConfigW(svc, nullptr, 0, &need);
  std::wstring image;
  if (need) {
    std::vector<uint8_t> buf(need);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (QueryServiceConfigW(svc, cfg, need, &need) && cfg->lpBinaryPathName) image = cfg->lpBinaryPathName;
  }
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return image;
}

std::wstring registered_sshd_exe() { return exe_from_image_path(service_image_path(L"sshd")); }

// True only if the sshd service points at this DustX folder's sshd.exe and that file is still there.
bool sshd_registration_ok() {
  if (!service_exists(L"sshd")) return false;
  const std::wstring got = registered_sshd_exe();
  if (!file_exists_w(got)) return false;
  return paths_same(got, bundled_sshd_exe());
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
  log_info("ssh", std::string("sshd 当前状态=") + service_state_name(st.dwCurrentState) + "(" +
                       std::to_string(st.dwCurrentState) + ")");
  if (st.dwCurrentState != SERVICE_RUNNING) {
    if (!StartServiceW(svc, 0, nullptr)) {
      const DWORD e = GetLastError();
      if (e != ERROR_SERVICE_ALREADY_RUNNING) {
        log_error("ssh", std::string("StartService 失败 ") + win_err(e) + "，登记路径=" +
                             wide_to_utf8(service_image_path(L"sshd")));
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err) *err = "无法启动 OpenSSH 服务（" + win_err(e) + "）。";
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
  log_info("ssh", std::string("sshd 启动结果 ") + service_state_name(st.dwCurrentState) +
                       (st.dwCurrentState == SERVICE_RUNNING ? "" : "（未进入运行中）"));
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
  } else if (service_exists(L"sshd") && !sshd_registration_ok()) {
    s.message = "OpenSSH 服务还在，但登记的 sshd.exe 已经不在当前尘埃X 目录。点「开启本机 SSH」，会按现在这份重新登记。";
  } else if (service_exists(L"sshd") && !service_running(L"sshd")) {
    s.message = "已安装 OpenSSH，但服务没开。点「开启本机 SSH」即可启动。";
  } else {
    s.message = "这台电脑还不能被 SSH 登录。点「开启本机 SSH」，尘埃X 会安装自带的 OpenSSH，不走 Windows 更新。";
  }
  return s;
}

SshHostStatus ssh_host_enable_work() {
  SshHostStatus s = ssh_host_probe();
  log_sshd_snapshot("开始开启");
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

  std::string err;
  if (sshd_registration_ok()) {
    log_info("ssh", "sshd 已登记为当前目录：" + wide_to_utf8(bundled_sshd_exe()));
  } else {
    const std::wstring got = registered_sshd_exe();
    log_info("ssh", "sshd 登记无效（已登记=" + (got.empty() ? std::string("无") : wide_to_utf8(got)) +
                         "，当前=" + wide_to_utf8(bundled_sshd_exe()) + "），按当前尘埃X 重新登记");
    if (!install_bundled_openssh(&err)) {
      log_sshd_snapshot("安装脚本失败");
      s.ok = false;
      s.message = err;
      return s;
    }
    log_sshd_snapshot("安装脚本结束");
  }
  if (!start_sshd(&err)) {
    log_sshd_snapshot("启动失败");
    s.ok = false;
    s.message = err.empty() ? "无法启动 OpenSSH 服务。" : err;
    return s;
  }

  allow_ssh_firewall();
  s.ready = wait_port(22, 40);
  s.need_admin = false;
  s.username = current_user();
  if (!s.ready) {
    log_sshd_snapshot("服务起来了但 22 未监听");
    s.ok = false;
    s.message = "OpenSSH 服务已启动，但 22 端口还没在听。请看下方日志。";
    return s;
  }
  log_sshd_snapshot("开启成功");
  s.ok = true;
  s.message = "本机 SSH 已开启。对方连上后，在对方电脑用这个用户名登录。";
  return s;
}

}  // namespace dustx
