#include "device_info.hpp"

#include "device_fp.hpp"
#include "util.hpp"
#include "version.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/mount.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

namespace dustx {
namespace {

struct Disk {
  std::string name;
  std::string path;
  uint64_t total = 0;
  uint64_t used = 0;
  std::string kind;
};

struct Info {
  std::string hostname;
  std::string model;
  std::string cpu;
  int cpu_cores = 0;
  std::string cpu_arch;
  uint64_t ram_bytes = 0;
  uint64_t ram_used_bytes = 0;
  std::vector<Disk> disks;
  std::string board;
  std::string gpu;
  std::string os;
  std::string os_build;
  uint64_t uptime_sec = 0;
};

std::string clip(std::string s, size_t n = 160) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  if (s.size() > n) s.resize(n);
  return s;
}

std::string json_u64(uint64_t v) {
  std::ostringstream o;
  o << v;
  return o.str();
}

#ifdef _WIN32

std::string wide_clip(const wchar_t* w, size_t n = 160) {
  if (!w || !*w) return {};
  return clip(wide_to_utf8(w), n);
}

std::string reg_sz(HKEY root, const wchar_t* path, const wchar_t* name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return {};
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  DWORD type = 0;
  const LONG st = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
  return wide_clip(buf);
}

void windows_smbios(std::string* model, std::string* board) {
  DWORD need = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
  if (!need) return;
  std::vector<uint8_t> buf(need);
  if (GetSystemFirmwareTable('RSMB', 0, buf.data(), need) != need || buf.size() < 8) return;
  const uint32_t length = *reinterpret_cast<const uint32_t*>(buf.data() + 4);
  if (8 + length > buf.size()) return;
  const uint8_t* data = buf.data() + 8;
  size_t off = 0;
  auto str_at = [](const char* strings, uint8_t index) -> std::string {
    if (!index || !strings) return {};
    const char* p = strings;
    for (uint8_t i = 1; i < index; ++i) {
      if (!*p) return {};
      p += std::strlen(p) + 1;
    }
    return clip(p);
  };
  while (off + 4 <= length) {
    const uint8_t type = data[off];
    const uint8_t slen = data[off + 1];
    if (slen < 4 || off + slen > length) break;
    const char* strings = reinterpret_cast<const char*>(data + off + slen);
    if (type == 1 && slen >= 8 && model->empty()) *model = str_at(strings, data[off + 4]);
    if (type == 2 && slen >= 8) {
      if (board->empty()) *board = str_at(strings, data[off + 5]);  // product
      if (board->empty()) *board = str_at(strings, data[off + 7]);  // serial
    }
    const char* p = strings;
    const uint8_t* end = data + length;
    while (reinterpret_cast<const uint8_t*>(p) + 1 < end && !(*p == 0 && *(p + 1) == 0)) ++p;
    off = static_cast<size_t>(reinterpret_cast<const uint8_t*>(p + 2) - data);
  }
}

Info collect_platform() {
  Info info;
  wchar_t host[256];
  DWORD n = 256;
  if (GetComputerNameExW(ComputerNameDnsHostname, host, &n)) info.hostname = wide_clip(host);
  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  info.cpu_cores = static_cast<int>(si.dwNumberOfProcessors);
  info.cpu_arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x64";
  info.cpu = clip(reg_sz(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         L"ProcessorNameString"));
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    info.ram_bytes = static_cast<uint64_t>(mem.ullTotalPhys);
    info.ram_used_bytes = static_cast<uint64_t>(mem.ullTotalPhys - mem.ullAvailPhys);
  }
  windows_smbios(&info.model, &info.board);
  const std::string product = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
  const std::string display = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
  const std::string build = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
  info.os = clip(product + (display.empty() ? "" : " " + display));
  info.os_build = clip(build);
  DISPLAY_DEVICEW dd{};
  dd.cb = sizeof(dd);
  std::string gpus;
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
    if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
    if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
    const std::string name = wide_clip(dd.DeviceString);
    if (name.empty()) continue;
    if (gpus.find(name) != std::string::npos) continue;
    if (!gpus.empty()) gpus += " / ";
    gpus += name;
  }
  info.gpu = clip(gpus, 200);
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(mask & (1u << i))) continue;
    wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
    const UINT type = GetDriveTypeW(root);
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
    ULARGE_INTEGER free_bytes{}, total_bytes{}, total_free{};
    if (!GetDiskFreeSpaceExW(root, &free_bytes, &total_bytes, &total_free) || total_bytes.QuadPart == 0) continue;
    Disk d;
    d.name = std::string(1, static_cast<char>('A' + i)) + ":";
    d.path = d.name + "\\";
    d.total = static_cast<uint64_t>(total_bytes.QuadPart);
    d.used = d.total - static_cast<uint64_t>(free_bytes.QuadPart);
    d.kind = type == DRIVE_REMOVABLE ? "removable" : "fixed";
    info.disks.push_back(d);
  }
  info.uptime_sec = GetTickCount64() / 1000;
  return info;
}

#else

std::string sysctl_str(const char* name) {
  size_t n = 0;
  if (sysctlbyname(name, nullptr, &n, nullptr, 0) != 0 || n == 0) return {};
  std::string buf(n, '\0');
  if (sysctlbyname(name, buf.data(), &n, nullptr, 0) != 0) return {};
  if (!buf.empty() && buf.back() == '\0') buf.pop_back();
  return clip(buf);
}

uint64_t sysctl_u64(const char* name) {
  uint64_t v = 0;
  size_t n = sizeof(v);
  if (sysctlbyname(name, &v, &n, nullptr, 0) == 0) return v;
  unsigned u = 0;
  n = sizeof(u);
  if (sysctlbyname(name, &u, &n, nullptr, 0) == 0) return u;
  return 0;
}

std::string cf_str(CFTypeRef ref) {
  if (!ref) return {};
  if (CFGetTypeID(ref) == CFStringGetTypeID()) {
    char buf[256];
    if (CFStringGetCString(static_cast<CFStringRef>(ref), buf, sizeof(buf), kCFStringEncodingUTF8)) return clip(buf);
  }
  if (CFGetTypeID(ref) == CFDataGetTypeID()) {
    CFDataRef data = static_cast<CFDataRef>(ref);
    const CFIndex n = CFDataGetLength(data);
    if (n <= 0 || n > 200) return {};
    std::string s(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<size_t>(n));
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return clip(s);
  }
  return {};
}

std::string ioreg_expert(const char* key) {
  io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
  if (!service) return {};
  CFStringRef cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
  CFTypeRef value = IORegistryEntryCreateCFProperty(service, cfkey, kCFAllocatorDefault, 0);
  CFRelease(cfkey);
  IOObjectRelease(service);
  std::string out = cf_str(value);
  if (value) CFRelease(value);
  return out;
}

std::string macos_gpu() {
  std::string names;
  auto take = [&](const char* match, const char* prop) {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(match), &it) != KERN_SUCCESS) return;
    io_object_t obj = 0;
    while ((obj = IOIteratorNext(it))) {
      CFStringRef cfkey = CFStringCreateWithCString(kCFAllocatorDefault, prop, kCFStringEncodingUTF8);
      CFTypeRef value = IORegistryEntryCreateCFProperty(obj, cfkey, kCFAllocatorDefault, 0);
      CFRelease(cfkey);
      const std::string name = cf_str(value);
      if (value) CFRelease(value);
      IOObjectRelease(obj);
      if (name.empty()) continue;
      if (names.find(name) != std::string::npos) continue;
      if (!names.empty()) names += " / ";
      names += name;
    }
    IOObjectRelease(it);
  };
  take("IOAccelerator", "model");
  if (names.empty()) take("AGXAccelerator", "model");
  return clip(names, 200);
}

bool skip_mount(const char* fstype, const char* mount) {
  const std::string t = fstype ? fstype : "";
  const std::string m = mount ? mount : "";
  if (t == "devfs" || t == "autofs" || t == "dev" || t == "fdesc") return true;
  if (m == "/dev" || m.rfind("/dev/", 0) == 0) return true;
  if (m.rfind("/System/Volumes/Preboot", 0) == 0) return true;
  if (m.rfind("/System/Volumes/VM", 0) == 0) return true;
  if (m.rfind("/System/Volumes/Update", 0) == 0) return true;
  if (m.rfind("/System/Volumes/xarts", 0) == 0) return true;
  if (m.rfind("/System/Volumes/iSCPreboot", 0) == 0) return true;
  if (m.rfind("/System/Volumes/Hardware", 0) == 0) return true;
  if (m == "/private/var/vm") return true;
  return false;
}

Info collect_platform() {
  Info info;
  char host[256];
  if (gethostname(host, sizeof(host)) == 0) info.hostname = clip(host);
  info.model = ioreg_expert("model");
  if (info.model.empty()) info.model = sysctl_str("hw.model");
  info.cpu = sysctl_str("machdep.cpu.brand_string");
  info.cpu_cores = static_cast<int>(sysctl_u64("hw.ncpu"));
  utsname un{};
  if (uname(&un) == 0) info.cpu_arch = clip(un.machine);
  info.ram_bytes = sysctl_u64("hw.memsize");
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  vm_size_t page = 0;
  host_page_size(mach_host_self(), &page);
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS &&
      page) {
    const uint64_t used_pages = static_cast<uint64_t>(vm.active_count) + vm.wire_count + vm.compressor_page_count;
    info.ram_used_bytes = used_pages * static_cast<uint64_t>(page);
    if (info.ram_bytes && info.ram_used_bytes > info.ram_bytes) info.ram_used_bytes = info.ram_bytes;
  }
  info.board = clip(ioreg_expert("board-id"));
  if (info.board.empty()) info.board = clip(ioreg_expert("IOPlatformSerialNumber"));
  info.gpu = macos_gpu();
  if (info.gpu.empty()) info.gpu = info.cpu;
  const std::string ver = sysctl_str("kern.osproductversion");
  info.os = clip(std::string("macOS ") + ver);
  info.os_build = sysctl_str("kern.osversion");
  struct timeval boot {};
  size_t boot_n = sizeof(boot);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &boot, &boot_n, nullptr, 0) == 0 && boot.tv_sec) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (now > boot.tv_sec) info.uptime_sec = static_cast<uint64_t>(now - boot.tv_sec);
  }
  struct statfs* mnt = nullptr;
  const int n = getmntinfo(&mnt, MNT_NOWAIT);
  for (int i = 0; i < n; ++i) {
    if (skip_mount(mnt[i].f_fstypename, mnt[i].f_mntonname)) continue;
    const uint64_t total = static_cast<uint64_t>(mnt[i].f_blocks) * mnt[i].f_bsize;
    const uint64_t freeb = static_cast<uint64_t>(mnt[i].f_bavail) * mnt[i].f_bsize;
    if (!total) continue;
    Disk d;
    d.path = mnt[i].f_mntonname;
    d.name = d.path == "/" ? "Macintosh HD" : d.path;
    auto slash = d.name.rfind('/');
    if (slash != std::string::npos && slash + 1 < d.name.size() && d.path != "/") d.name = d.name.substr(slash + 1);
    d.total = total;
    d.used = total > freeb ? total - freeb : 0;
    d.kind = (std::strcmp(mnt[i].f_fstypename, "apfs") == 0) ? "ssd" : mnt[i].f_fstypename;
    bool dup = false;
    for (const auto& prev : info.disks) {
      if (prev.total == d.total && prev.used == d.used) dup = true;
    }
    if (!dup) info.disks.push_back(d);
  }
  return info;
}

#endif

std::string to_json(const Info& info) {
  std::ostringstream o;
  o << "{\"ok\":true"
    << ",\"version\":\"" << json_escape(kAppVersion) << "\""
    << ",\"hostname\":\"" << json_escape(info.hostname) << "\""
    << ",\"model\":\"" << json_escape(info.model) << "\""
    << ",\"cpu\":\"" << json_escape(info.cpu) << "\""
    << ",\"cpu_cores\":" << info.cpu_cores
    << ",\"cpu_arch\":\"" << json_escape(info.cpu_arch) << "\""
    << ",\"ram_bytes\":" << json_u64(info.ram_bytes)
    << ",\"ram_used_bytes\":" << json_u64(info.ram_used_bytes)
    << ",\"board\":\"" << json_escape(info.board) << "\""
    << ",\"gpu\":\"" << json_escape(info.gpu) << "\""
    << ",\"os\":\"" << json_escape(info.os) << "\""
    << ",\"os_build\":\"" << json_escape(info.os_build) << "\""
    << ",\"uptime_sec\":" << json_u64(info.uptime_sec)
    << ",\"disks\":[";
  for (size_t i = 0; i < info.disks.size(); ++i) {
    if (i) o << ",";
    const Disk& d = info.disks[i];
    o << "{\"name\":\"" << json_escape(d.name) << "\",\"path\":\"" << json_escape(d.path)
      << "\",\"total\":" << json_u64(d.total) << ",\"used\":" << json_u64(d.used) << ",\"kind\":\""
      << json_escape(d.kind) << "\"}";
  }
  o << "]}";
  return o.str();
}

}  // namespace

std::string app_version() { return kAppVersion; }

std::string device_info_json() {
  static std::mutex mu;
  static std::string cached;
  static std::chrono::steady_clock::time_point at;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::steady_clock::now();
  if (!cached.empty() && now - at < std::chrono::seconds(2)) return cached;
  cached = to_json(collect_platform());
  at = now;
  return cached;
}

}  // namespace dustx
