#include "virtual_io.hpp"

#include "log.hpp"
#include "vcam_install.hpp"
#include "vmic_shm.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dustx {

struct VirtualIO::Impl {
  VmicShm mic;
  std::thread test_thread;
  bool stop_test = false;
  bool got_video = false;
  bool got_audio = false;
};

VirtualIO::~VirtualIO() { stop(); }

bool VirtualIO::running() const {
  std::lock_guard<std::mutex> lock(mu_);
  return running_;
}

std::string VirtualIO::message() const {
  std::lock_guard<std::mutex> lock(mu_);
  return message_;
}

bool VirtualIO::start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return true;
  if (!impl_) impl_ = new Impl();
  cam_ok_ = open_camera();
  mic_ok_ = open_mic();
  running_ = true;
  log_info("vio", std::string("虚拟设备输出 摄像头=") + (cam_ok_ ? "开" : "关") + " 麦克风=" + (mic_ok_ ? "开" : "关"));
  impl_->stop_test = false;
  impl_->got_video = false;
  impl_->got_audio = false;
  impl_->test_thread = std::thread([this] {
    uint32_t tick = 0;
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_ || !impl_ || impl_->stop_test) break;
      }
      bool need_video = false;
      bool need_audio = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        need_video = cam_ok_ && impl_ && !impl_->got_video;
        need_audio = mic_ok_ && impl_ && !impl_->got_audio;
      }
      if (need_video) {
        std::vector<uint8_t> rgb(static_cast<size_t>(kVcamWidth * kVcamHeight * 3));
        for (int y = 0; y < kVcamHeight; ++y) {
          for (int x = 0; x < kVcamWidth; ++x) {
            const int i = (y * kVcamWidth + x) * 3;
            rgb[i] = static_cast<uint8_t>((x + tick) & 255);
            rgb[i + 1] = static_cast<uint8_t>((y + tick / 2) & 255);
            rgb[i + 2] = 64;
          }
        }
        vcam_.write_rgb(rgb.data(), kVcamWidth, kVcamHeight);
      }
      if (need_audio) {
        float tone[960];
        for (int i = 0; i < 960; ++i) {
          tone[i] = 0.12f * std::sin(2.f * 3.1415926f * 440.f * (static_cast<float>(tick * 960 + i) / 48000.f));
        }
        impl_->mic.write_pcm(tone, 960);
      }
      tick++;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });
  if (cam_ok_ && mic_ok_) {
    message_ = cam_note_.empty() ? "已输出到「尘埃 摄像头」和「尘埃 麦克风」。OBS / 会议软件请选这两个设备。"
                                 : cam_note_ + " 麦克风请选「尘埃 麦克风」。";
  } else if (cam_ok_) {
    message_ = cam_note_.empty() ? "尘埃 摄像头已打开，虚拟麦克风还没装上。" : cam_note_;
  } else if (mic_ok_) {
    message_ = "尘埃 麦克风已打开，摄像头扩展还没被系统认到。";
  } else {
    message_ = "虚拟设备没有打开。窗口预览仍可用。";
  }
  return true;
}

void VirtualIO::stop() {
  std::thread dying;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return;
    if (impl_) impl_->stop_test = true;
    running_ = false;
    if (impl_ && impl_->test_thread.joinable()) dying.swap(impl_->test_thread);
    close_mic();
    close_camera();
    cam_ok_ = false;
    mic_ok_ = false;
    message_ = "已停止虚拟设备输出";
  }
  if (dying.joinable()) dying.join();
  std::lock_guard<std::mutex> lock(mu_);
  delete impl_;
  impl_ = nullptr;
}

void VirtualIO::send_rgb(const uint8_t* rgb, int width, int height) {
  std::lock_guard<std::mutex> lock(mu_);
  width_ = width;
  height_ = height;
  if (impl_) impl_->got_video = true;
  if (!running_ || !cam_ok_ || !rgb) return;
  vcam_.write_rgb(rgb, width, height);
}

void VirtualIO::send_pcm(const float* samples, int count) {
  if (!samples || count <= 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (impl_) impl_->got_audio = true;
  if (!running_ || !mic_ok_ || !impl_) return;
  impl_->mic.write_pcm(samples, count);
}

bool VirtualIO::open_camera() {
  cam_note_.clear();
  install_vcam(&cam_note_);
  return vcam_.open_writer();
}

void VirtualIO::close_camera() { vcam_.close(); }

bool VirtualIO::open_mic() {
  std::string note;
  install_vmic(&note);
  if (!impl_) impl_ = new Impl();
  if (!impl_->mic.open_writer()) {
    if (!note.empty()) cam_note_ = (cam_note_.empty() ? "" : cam_note_ + " ") + note;
    return false;
  }
  if (!note.empty()) cam_note_ = (cam_note_.empty() ? "" : cam_note_ + " ") + note;
  return true;
}

void VirtualIO::close_mic() {
  if (impl_) impl_->mic.close();
}

}  // namespace dustx
