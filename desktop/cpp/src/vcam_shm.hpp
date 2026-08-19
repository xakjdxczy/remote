#pragma once

#include <cstdint>
#include <cstddef>

namespace dustx {

constexpr uint32_t kVcamMagic = 0x44585643;  // DXVC
constexpr uint32_t kVcamVersion = 1;
constexpr int kVcamWidth = 1280;
constexpr int kVcamHeight = 720;
constexpr int kVcamFps = 30;
constexpr int kVcamHeaderOff = 4096;
constexpr int kVcamMaxBytes = kVcamWidth * kVcamHeight * 3;
constexpr int kVcamShmBytes = kVcamHeaderOff + kVcamMaxBytes;

#ifdef _WIN32
constexpr const wchar_t* kVcamWinName = L"Local\\DustXVirtualCam";
#endif
constexpr const char* kVcamFilePath = "/tmp/dustx-vcam";

#pragma pack(push, 8)
struct VcamHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t fps;
  uint32_t frame_id;
  uint32_t ready;
};
#pragma pack(pop)

class VcamShm {
 public:
  ~VcamShm();
  bool open_writer();
  bool open_reader();
  void close();
  bool valid() const { return view_ != nullptr; }
  void write_rgb(const uint8_t* rgb, int width, int height);
  bool read_rgb(uint8_t* rgb, int* width, int* height, uint32_t* frame_id) const;
  VcamHeader* header() const { return reinterpret_cast<VcamHeader*>(view_); }
  uint8_t* pixels() const {
    return view_ ? reinterpret_cast<uint8_t*>(view_) + kVcamHeaderOff : nullptr;
  }

 private:
  bool map(bool create);
  void* view_ = nullptr;
  void* handle_ = nullptr;
  int fd_ = -1;
};

void scale_rgb_nn(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh);

}  // namespace dustx
