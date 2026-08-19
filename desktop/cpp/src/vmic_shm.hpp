#pragma once

#include <cstdint>
#include <cstddef>

namespace dustx {

constexpr uint32_t kVmicMagic = 0x4458564D;  // DXVM
constexpr uint32_t kVmicVersion = 1;
constexpr int kVmicRate = 48000;
constexpr int kVmicChannels = 1;
constexpr int kVmicCapacity = kVmicRate * 2;
constexpr int kVmicHeaderOff = 4096;
constexpr int kVmicShmBytes = kVmicHeaderOff + static_cast<int>(kVmicCapacity * sizeof(float));

#ifdef _WIN32
constexpr const wchar_t* kVmicWinName = L"Local\\DustXVirtualMic";
#endif
constexpr const char* kVmicFilePath = "/tmp/dustx-vmic";

#pragma pack(push, 8)
struct VmicHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t write_idx;
  uint32_t ready;
};
#pragma pack(pop)

class VmicShm {
 public:
  ~VmicShm();
  bool open_writer();
  bool open_reader();
  void close();
  bool valid() const { return view_ != nullptr; }
  void write_pcm(const float* samples, int count);
  int read_pcm(float* samples, int count);
  VmicHeader* header() const { return reinterpret_cast<VmicHeader*>(view_); }
  float* samples() const {
    return view_ ? reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(view_) + kVmicHeaderOff) : nullptr;
  }

 private:
  bool map(bool create);
  void* view_ = nullptr;
  void* handle_ = nullptr;
  int fd_ = -1;
  uint32_t read_idx_ = 0;
};

}  // namespace dustx
