#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dshow.h>

#include "vcam_install.hpp"

#include <string>

#pragma comment(lib, "strmiids.lib")

namespace {

constexpr wchar_t kCamClsid[] = L"{A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F70}";
constexpr wchar_t kMicClsid[] = L"{A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F81}";
constexpr wchar_t kVideoCat[] = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";
constexpr wchar_t kAudioCat[] = L"{33D9A762-90C8-11d0-BD43-00A0C911CE86}";

std::wstring exe_dir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring p(exe);
  auto slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) p.resize(slash + 1);
  return p;
}

bool write_sz(HKEY root, const wchar_t* path, const wchar_t* name, const wchar_t* value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  LONG ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                           static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return ok == ERROR_SUCCESS;
}

bool register_dshow(const wchar_t* clsid, const wchar_t* category, const wchar_t* name, const wchar_t* dll,
                    const CLSID& clsid_guid, const CLSID& cat_guid, const GUID& major, const GUID& minor) {
  std::wstring clsid_path = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
  std::wstring inproc = clsid_path + L"\\InprocServer32";
  std::wstring inst = std::wstring(L"Software\\Classes\\CLSID\\") + category + L"\\Instance\\" + clsid;
  bool ok = write_sz(HKEY_CURRENT_USER, clsid_path.c_str(), nullptr, name);
  ok = write_sz(HKEY_CURRENT_USER, inproc.c_str(), nullptr, dll) && ok;
  ok = write_sz(HKEY_CURRENT_USER, inproc.c_str(), L"ThreadingModel", L"Both") && ok;
  ok = write_sz(HKEY_CURRENT_USER, inst.c_str(), L"FriendlyName", name) && ok;
  ok = write_sz(HKEY_CURRENT_USER, inst.c_str(), L"CLSID", clsid) && ok;

  IFilterMapper2* mapper = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                                 reinterpret_cast<void**>(&mapper))) &&
      mapper) {
    REGPINTYPES types{};
    types.clsMajorType = &major;
    types.clsMinorType = &minor;
    REGFILTERPINS pin{};
    pin.bOutput = TRUE;
    pin.nMediaTypes = 1;
    pin.lpMediaType = &types;
    REGFILTER2 rf2{};
    rf2.dwVersion = 1;
    rf2.dwMerit = MERIT_DO_NOT_USE;
    rf2.cPins = 1;
    rf2.rgPins = &pin;
    mapper->RegisterFilter(clsid_guid, name, nullptr, &cat_guid, nullptr, &rf2);
    mapper->Release();
  }
  return ok;
}

}  // namespace

namespace dustx {

bool install_vcam(std::string* message) {
  const std::wstring dll = exe_dir() + L"DustXCam.dll";
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (message) *message = "找不到 DustXCam.dll，虚拟摄像头没有打进程序目录。";
    return false;
  }
  const bool ok = register_dshow(kCamClsid, kVideoCat, L"尘埃 摄像头", dll.c_str(),
                                 {0xa71c3e80, 0x6d42, 0x4f1b, {0x9e, 0x3a, 0xc4, 0xd8, 0xb2, 0xe9, 0x1f, 0x70}},
                                 {0x860BB310, 0x5D01, 0x11d0, {0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86}},
                                 MEDIATYPE_Video, MEDIASUBTYPE_RGB24);
  if (message) {
    *message = ok ? "已注册尘埃 摄像头。OBS 选「视频采集设备 → 尘埃 摄像头」。"
                  : "注册虚拟摄像头失败。";
  }
  return ok;
}

bool install_vmic(std::string* message) {
  const std::wstring dll = exe_dir() + L"DustXMic.dll";
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (message) *message = "找不到 DustXMic.dll，虚拟麦克风没有打进程序目录。";
    return false;
  }
  const bool ok = register_dshow(kMicClsid, kAudioCat, L"尘埃 麦克风", dll.c_str(),
                                 {0xa71c3e80, 0x6d42, 0x4f1b, {0x9e, 0x3a, 0xc4, 0xd8, 0xb2, 0xe9, 0x1f, 0x81}},
                                 {0x33D9A762, 0x90C8, 0x11d0, {0xBD, 0x43, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86}},
                                 MEDIATYPE_Audio, MEDIASUBTYPE_PCM);
  if (message) {
    *message = ok ? "已注册尘埃 麦克风。OBS 视频采集设备里可把音频选成它。"
                  : "注册虚拟麦克风失败。";
  }
  return ok;
}

}  // namespace dustx
