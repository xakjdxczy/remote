#include "vmic_shm.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dustx {

VmicShm::~VmicShm() { close(); }

bool VmicShm::open_writer() { return map(true); }
bool VmicShm::open_reader() { return map(false); }

#ifdef _WIN32
bool VmicShm::map(bool create) {
  if (view_) return true;
  HANDLE h = nullptr;
  if (create) {
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kVmicShmBytes, kVmicWinName);
  } else {
    h = OpenFileMappingW(FILE_MAP_READ, FALSE, kVmicWinName);
  }
  if (!h) return false;
  void* view = MapViewOfFile(h, create ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, kVmicShmBytes);
  if (!view) {
    CloseHandle(h);
    return false;
  }
  handle_ = h;
  view_ = view;
  if (create) {
    auto* hdr = header();
    if (hdr->magic != kVmicMagic) {
      std::memset(view_, 0, kVmicShmBytes);
      hdr->magic = kVmicMagic;
      hdr->version = kVmicVersion;
      hdr->sample_rate = kVmicRate;
      hdr->channels = kVmicChannels;
    }
  }
  return true;
}

void VmicShm::close() {
  if (view_) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (handle_) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}
#else
bool VmicShm::map(bool create) {
  if (view_) return true;
  int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
  int fd = ::open(kVmicFilePath, flags, 0666);
  if (fd < 0) return false;
  if (create) {
    if (ftruncate(fd, kVmicShmBytes) != 0) {
      ::close(fd);
      return false;
    }
    ::chmod(kVmicFilePath, 0666);
  }
  void* view = mmap(nullptr, kVmicShmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (view == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  view_ = view;
  if (create) {
    auto* hdr = header();
    if (hdr->magic != kVmicMagic) {
      std::memset(view_, 0, kVmicShmBytes);
      hdr->magic = kVmicMagic;
      hdr->version = kVmicVersion;
      hdr->sample_rate = kVmicRate;
      hdr->channels = kVmicChannels;
    }
  }
  return true;
}

void VmicShm::close() {
  if (view_) {
    munmap(view_, kVmicShmBytes);
    view_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}
#endif

void VmicShm::write_pcm(const float* samples, int count) {
  if (!view_ || !samples || count <= 0) return;
  auto* hdr = header();
  float* buf = this->samples();
  uint32_t idx = hdr->write_idx;
  for (int i = 0; i < count; ++i) {
    buf[idx % kVmicCapacity] = samples[i];
    idx++;
  }
  hdr->magic = kVmicMagic;
  hdr->version = kVmicVersion;
  hdr->sample_rate = kVmicRate;
  hdr->channels = kVmicChannels;
  hdr->write_idx = idx;
  hdr->ready = 1;
}

int VmicShm::read_pcm(float* out, int count) {
  if (!view_ || !out || count <= 0) return 0;
  const auto* hdr = header();
  if (hdr->magic != kVmicMagic || !hdr->ready) {
    std::memset(out, 0, static_cast<size_t>(count) * sizeof(float));
    return 0;
  }
  const float* buf = samples();
  uint32_t write = hdr->write_idx;
  if (write - read_idx_ > static_cast<uint32_t>(kVmicCapacity)) {
    read_idx_ = write - static_cast<uint32_t>(kVmicCapacity / 2);
  }
  int got = 0;
  while (got < count && read_idx_ < write) {
    out[got++] = buf[read_idx_ % kVmicCapacity];
    read_idx_++;
  }
  while (got < count) out[got++] = 0.f;
  return got;
}

}  // namespace dustx
