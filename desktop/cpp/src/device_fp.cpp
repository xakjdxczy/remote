#include "device_fp.hpp"

#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ipifcons.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/socket.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

namespace dustx {
namespace {

std::string trim_copy(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

std::string upper_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool is_placeholder(const std::string& s) {
  std::string u = upper_copy(trim_copy(s));
  std::string compact;
  for (char c : u) {
    if (c != ' ' && c != '-' && c != '_' && c != '.') compact.push_back(c);
  }
  return u.empty() || u == "NONE" || u == "NULL" || u == "N/A" || u == "NA" || u == "UNKNOWN" ||
         u == "DEFAULT STRING" || compact == "DEFAULTSTRING" || u == "TO BE FILLED BY O.E.M." ||
         compact == "TOBEFILLEDBYOEM" || u == "TO BE FILLED" || u == "SYSTEM SERIAL NUMBER" ||
         u == "0";
}

std::string normalize_board(std::string s) {
  s = upper_copy(trim_copy(s));
  return is_placeholder(s) ? "" : s;
}

std::string normalize_nic(const std::string& s) {
  std::string hex;
  for (char c : s) {
    if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (hex.size() != 12) return "";
  if (hex == "000000000000" || hex == "ffffffffffff") return "";
  return hex;
}

std::string normalize_uuid(const std::string& s) {
  std::string hex;
  for (char c : s) {
    if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (hex.size() != 32) return "";
  if (hex == std::string(32, '0') || hex == std::string(32, 'f')) return "";
  return hex;
}

std::string nic_display(const std::string& hex) {
  if (hex.size() != 12) return hex;
  std::string out;
  for (size_t i = 0; i < 12; i += 2) {
    if (i) out.push_back(':');
    out.append(hex, i, 2);
  }
  return out;
}

std::string uuid_display(const std::string& hex) {
  if (hex.size() != 32) return hex;
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) +
         "-" + hex.substr(20);
}

#ifdef _WIN32

bool looks_virtual(const std::string& name) {
  std::string u = upper_copy(name);
  return u.find("WINTUN") != std::string::npos || u.find("TAP-") != std::string::npos ||
         u.find("TUNNEL") != std::string::npos || u.find("VPN") != std::string::npos ||
         u.find("HYPER-V") != std::string::npos || u.find("VIRTUAL") != std::string::npos ||
         u.find("VMWARE") != std::string::npos || u.find("VIRTUALBOX") != std::string::npos ||
         u.find("BLUETOOTH") != std::string::npos || u.find("LOOPBACK") != std::string::npos ||
         u.find("TAILSCALE") != std::string::npos || u.find("WIREGUARD") != std::string::npos ||
         u.find("DUSTX") != std::string::npos || u.find("UTUN") != std::string::npos;
}

std::string smbios_string(const char* strings, uint8_t index) {
  if (!index || !strings) return "";
  const char* p = strings;
  for (uint8_t i = 1; i < index; ++i) {
    if (!*p) return "";
    p += std::strlen(p) + 1;
  }
  return p;
}

void parse_smbios(std::string* board, std::string* uuid) {
  DWORD need = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
  if (!need) return;
  std::vector<uint8_t> buf(need);
  if (GetSystemFirmwareTable('RSMB', 0, buf.data(), need) != need || buf.size() < 8) return;
  const uint32_t length = *reinterpret_cast<const uint32_t*>(buf.data() + 4);
  if (8 + length > buf.size()) return;
  const uint8_t* data = buf.data() + 8;
  size_t off = 0;
  while (off + 4 <= length) {
    const uint8_t type = data[off];
    const uint8_t slen = data[off + 1];
    if (slen < 4 || off + slen > length) break;
    const char* strings = reinterpret_cast<const char*>(data + off + slen);
    if (type == 1 && slen >= 24 && uuid->empty()) {
      const uint8_t* raw = data + off + 8;
      bool blank = true, ones = true;
      for (int i = 0; i < 16; ++i) {
        if (raw[i] != 0) blank = false;
        if (raw[i] != 0xff) ones = false;
      }
      if (!blank && !ones) {
        char hex[33];
        static const char* kDigits = "0123456789abcdef";
        // SMBIOS 2.6+ mixed-endian UUID
        const uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
        for (int i = 0; i < 16; ++i) {
          hex[i * 2] = kDigits[(raw[order[i]] >> 4) & 0xf];
          hex[i * 2 + 1] = kDigits[raw[order[i]] & 0xf];
        }
        hex[32] = 0;
        *uuid = hex;
      }
    }
    if (type == 2 && slen >= 8 && board->empty()) {
      *board = smbios_string(strings, data[off + 7]);
    }
    const char* p = strings;
    const uint8_t* end = data + length;
    while (reinterpret_cast<const uint8_t*>(p) + 1 < end && !(*p == 0 && *(p + 1) == 0)) ++p;
    off = static_cast<size_t>(reinterpret_cast<const uint8_t*>(p + 2) - data);
  }
}

std::string windows_nic() {
  ULONG size = 0;
  if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                           nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
    return "";
  }
  std::vector<uint8_t> buf(size);
  auto* head = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                           nullptr, head, &size) != NO_ERROR) {
    return "";
  }
  std::string best;
  int best_score = -1;
  for (auto* a = head; a; a = a->Next) {
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
    if (a->PhysicalAddressLength != 6) continue;
    std::string desc, name;
    if (a->Description) {
      for (const wchar_t* p = a->Description; *p; ++p) {
        if (*p < 128) desc.push_back(static_cast<char>(*p));
      }
    }
    if (a->FriendlyName) {
      for (const wchar_t* p = a->FriendlyName; *p; ++p) {
        if (*p < 128) name.push_back(static_cast<char>(*p));
      }
    }
    if (looks_virtual(desc) || looks_virtual(name)) continue;
    char hex[13];
    static const char* kDigits = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
      hex[i * 2] = kDigits[(a->PhysicalAddress[i] >> 4) & 0xf];
      hex[i * 2 + 1] = kDigits[a->PhysicalAddress[i] & 0xf];
    }
    hex[12] = 0;
    std::string nic = normalize_nic(hex);
    if (nic.empty()) continue;
    int score = 0;
    if (a->OperStatus == IfOperStatusUp) score += 10;
    if (a->IfType == IF_TYPE_ETHERNET_CSMACD) score += 3;
    if (a->IfType == IF_TYPE_IEEE80211) score += 2;
    if (score > best_score) {
      best_score = score;
      best = nic;
    }
  }
  return best;
}

HardwareFingerprint collect_platform() {
  HardwareFingerprint fp;
  parse_smbios(&fp.board, &fp.uuid);
  fp.board = normalize_board(fp.board);
  fp.uuid = normalize_uuid(fp.uuid);
  fp.nic = windows_nic();
  return fp;
}

#else

std::string cf_string(CFTypeRef ref) {
  if (!ref || CFGetTypeID(ref) != CFStringGetTypeID()) return "";
  char buf[256];
  if (CFStringGetCString(static_cast<CFStringRef>(ref), buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
  return "";
}

std::string macos_ioreg(const char* key) {
  io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
  if (!service) return "";
  CFStringRef cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
  CFTypeRef value = IORegistryEntryCreateCFProperty(service, cfkey, kCFAllocatorDefault, 0);
  CFRelease(cfkey);
  IOObjectRelease(service);
  std::string out = cf_string(value);
  if (value) CFRelease(value);
  return out;
}

std::string macos_nic() {
  ifaddrs* addrs = nullptr;
  if (getifaddrs(&addrs) != 0) return "";
  std::string best;
  int best_score = -1;
  for (ifaddrs* a = addrs; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK) continue;
    if (!a->ifa_name) continue;
    std::string name = a->ifa_name;
    if (name == "lo0" || name.rfind("lo", 0) == 0) continue;
    if (name.rfind("utun", 0) == 0 || name.rfind("awdl", 0) == 0 || name.rfind("llw", 0) == 0) continue;
    if (name.rfind("bridge", 0) == 0 || name.rfind("gif", 0) == 0 || name.rfind("stf", 0) == 0) continue;
    if (name.rfind("vmnet", 0) == 0 || name.rfind("vmenet", 0) == 0) continue;
    auto* dl = reinterpret_cast<sockaddr_dl*>(a->ifa_addr);
    if (dl->sdl_alen != 6) continue;
    const uint8_t* mac = reinterpret_cast<const uint8_t*>(LLADDR(dl));
    char hex[13];
    static const char* kDigits = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
      hex[i * 2] = kDigits[(mac[i] >> 4) & 0xf];
      hex[i * 2 + 1] = kDigits[mac[i] & 0xf];
    }
    hex[12] = 0;
    std::string nic = normalize_nic(hex);
    if (nic.empty()) continue;
    int score = 0;
    if (a->ifa_flags & IFF_UP) score += 10;
    if (name == "en0") score += 5;
    else if (name.rfind("en", 0) == 0) score += 3;
    if (score > best_score) {
      best_score = score;
      best = nic;
    }
  }
  freeifaddrs(addrs);
  return best;
}

HardwareFingerprint collect_platform() {
  HardwareFingerprint fp;
  fp.board = normalize_board(macos_ioreg("IOPlatformSerialNumber"));
  fp.uuid = normalize_uuid(macos_ioreg("IOPlatformUUID"));
  fp.nic = macos_nic();
  return fp;
}

#endif

}  // namespace

bool HardwareFingerprint::complete() const { return !board.empty() && !nic.empty() && !uuid.empty(); }

std::string HardwareFingerprint::json() const {
  std::ostringstream o;
  o << "{\"board\":\"" << json_escape(board) << "\",\"nic\":\"" << json_escape(nic_display(nic))
    << "\",\"uuid\":\"" << json_escape(uuid_display(uuid)) << "\"}";
  return o.str();
}

HardwareFingerprint hardware_fingerprint() {
  static std::mutex mu;
  static HardwareFingerprint cached;
  static bool ready = false;
  std::lock_guard<std::mutex> lock(mu);
  if (!ready) {
    cached = collect_platform();
    ready = true;
  }
  return cached;
}

}  // namespace dustx
