#pragma once

#include <string>

namespace dustx {

struct HardwareFingerprint {
  std::string board;
  std::string nic;
  std::string uuid;
  bool complete() const;
  std::string json() const;
};

HardwareFingerprint hardware_fingerprint();

}  // namespace dustx
