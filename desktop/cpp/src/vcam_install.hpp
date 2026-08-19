#pragma once

#include <string>

namespace dustx {

// Register / activate our own "尘埃X 摄像头" device. Does not use OBS.
bool install_vcam(std::string* message);
// Install "尘埃X 麦克风" so meeting/OBS apps can capture phone audio.
bool install_vmic(std::string* message);

}  // namespace dustx
