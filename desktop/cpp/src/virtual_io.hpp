#pragma once

#include "vcam_shm.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace dustx {

class VirtualIO {
 public:
  ~VirtualIO();
  bool start();
  void stop();
  bool running() const;
  std::string message() const;
  void send_rgb(const uint8_t* rgb, int width, int height);
  void send_pcm(const float* samples, int count);

 private:
  bool open_camera();
  bool open_mic();
  void close_camera();
  void close_mic();

  mutable std::mutex mu_;
  bool running_ = false;
  bool cam_ok_ = false;
  bool mic_ok_ = false;
  std::string message_;
  int width_ = 0;
  int height_ = 0;
  VcamShm vcam_;
  std::string cam_note_;
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace dustx
