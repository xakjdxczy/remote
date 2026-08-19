#include "vcam_shm.hpp"

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

void scale_rgb_nn(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
  if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
  if (sw == dw && sh == dh) {
    std::memcpy(dst, src, static_cast<size_t>(dw * dh * 3));
    return;
  }
  for (int y = 0; y < dh; ++y) {
    const int sy = y * sh / dh;
    const uint8_t* srow = src + static_cast<size_t>(sy) * sw * 3;
    uint8_t* drow = dst + static_cast<size_t>(y) * dw * 3;
    for (int x = 0; x < dw; ++x) {
      const int sx = x * sw / dw;
      const uint8_t* p = srow + sx * 3;
      drow[x * 3] = p[0];
      drow[x * 3 + 1] = p[1];
      drow[x * 3 + 2] = p[2];
    }
  }
}

VcamShm::~VcamShm() { close(); }

bool VcamShm::open_writer() { return map(true); }
bool VcamShm::open_reader() { return map(false); }

#ifdef _WIN32
bool VcamShm::map(bool create) {
  if (view_) return true;
  HANDLE h = nullptr;
  if (create) {
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kVcamShmBytes, kVcamWinName);
  } else {
    h = OpenFileMappingW(FILE_MAP_READ, FALSE, kVcamWinName);
  }
  if (!h) return false;
  void* view = MapViewOfFile(h, create ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, kVcamShmBytes);
  if (!view) {
    CloseHandle(h);
    return false;
  }
  handle_ = h;
  view_ = view;
  if (create) {
    auto* hdr = header();
    if (hdr->magic != kVcamMagic) {
      std::memset(view_, 0, kVcamShmBytes);
      hdr->magic = kVcamMagic;
      hdr->version = kVcamVersion;
      hdr->width = kVcamWidth;
      hdr->height = kVcamHeight;
      hdr->stride = kVcamWidth * 3;
      hdr->fps = kVcamFps;
    }
  }
  return true;
}

void VcamShm::close() {
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
bool VcamShm::map(bool create) {
  if (view_) return true;
  int flags = create ? (O_RDWR | O_CREAT) : O_RDONLY;
  int fd = ::open(kVcamFilePath, flags, 0666);
  if (fd < 0) return false;
  if (create) {
    if (ftruncate(fd, kVcamShmBytes) != 0) {
      ::close(fd);
      return false;
    }
    ::chmod(kVcamFilePath, 0666);
  }
  void* view = mmap(nullptr, kVcamShmBytes, create ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, fd, 0);
  if (view == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  view_ = view;
  if (create) {
    auto* hdr = header();
    if (hdr->magic != kVcamMagic) {
      std::memset(view_, 0, kVcamShmBytes);
      hdr->magic = kVcamMagic;
      hdr->version = kVcamVersion;
      hdr->width = kVcamWidth;
      hdr->height = kVcamHeight;
      hdr->stride = kVcamWidth * 3;
      hdr->fps = kVcamFps;
    }
  }
  return true;
}

void VcamShm::close() {
  if (view_) {
    munmap(view_, kVcamShmBytes);
    view_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}
#endif

void VcamShm::write_rgb(const uint8_t* rgb, int width, int height) {
  if (!view_ || !rgb || width <= 0 || height <= 0) return;
  auto* hdr = header();
  scale_rgb_nn(rgb, width, height, pixels(), kVcamWidth, kVcamHeight);
  hdr->magic = kVcamMagic;
  hdr->version = kVcamVersion;
  hdr->width = kVcamWidth;
  hdr->height = kVcamHeight;
  hdr->stride = kVcamWidth * 3;
  hdr->fps = kVcamFps;
  hdr->frame_id += 1;
  hdr->ready = 1;
}

bool VcamShm::read_rgb(uint8_t* rgb, int* width, int* height, uint32_t* frame_id) const {
  if (!view_ || !rgb) return false;
  const auto* hdr = header();
  if (hdr->magic != kVcamMagic || !hdr->ready) return false;
  if (hdr->width != static_cast<uint32_t>(kVcamWidth) || hdr->height != static_cast<uint32_t>(kVcamHeight)) {
    return false;
  }
  std::memcpy(rgb, pixels(), kVcamMaxBytes);
  if (width) *width = kVcamWidth;
  if (height) *height = kVcamHeight;
  if (frame_id) *frame_id = hdr->frame_id;
  return true;
}

}  // namespace dustx
