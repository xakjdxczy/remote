#pragma once

#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dustx {

#ifdef _WIN32
void install_crash_handler();
std::string crash_dir();
bool write_minidump(EXCEPTION_POINTERS* ep, const char* why, bool wait = true);
#endif

}  // namespace dustx
