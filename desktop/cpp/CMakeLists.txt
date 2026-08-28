cmake_minimum_required(VERSION 3.16)
project(DustX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp" _DUSTX_VERSION_HPP)
if(NOT _DUSTX_VERSION_HPP MATCHES "kAppVersion = \"([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)\"")
  message(FATAL_ERROR "src/version.hpp is missing kAppVersion a.b.c.d")
endif()
set(DUSTX_APP_VERSION "${CMAKE_MATCH_1}")
string(REPLACE "." "," DUSTX_APP_VERSION_COMMA "${DUSTX_APP_VERSION}")
set(DUSTX_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/windows/app.manifest")
string(REPLACE "\\" "/" DUSTX_APP_MANIFEST "${DUSTX_APP_MANIFEST}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/windows/dustx.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/dustx.rc"
  @ONLY
)

set(DUSTX_COMMON
  src/main.cpp
  src/server.cpp
  src/cam_hub.cpp
  src/mesh.cpp
  src/device_fp.cpp
  src/device_info.cpp
  src/recents.cpp
  src/update.cpp
  src/net.cpp
  src/util.cpp
  src/log.cpp
  src/settings.cpp
  src/agent.cpp
  src/virtual_io.cpp
  src/vcam_shm.cpp
  src/vmic_shm.cpp
)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchPlatformTools.cmake")

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.3")

  add_executable(DustXCamera MACOSX_BUNDLE
    vcam/macos/DustXCameraExt.mm
    src/vcam_shm.cpp
  )
  set_target_properties(DustXCamera PROPERTIES
    OUTPUT_NAME "DustXCamera"
    BUNDLE TRUE
    BUNDLE_EXTENSION systemextension
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/vcam/macos/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.dustx.remotedesk.camera"
    MACOSX_BUNDLE_BUNDLE_NAME "尘埃X 摄像头"
  )
  target_link_libraries(DustXCamera PRIVATE
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreMediaIO"
    "-framework CoreVideo"
  )
  target_compile_options(DustXCamera PRIVATE -fobjc-arc)

  add_library(DustXMic MODULE vmic/macos/dustx_mic.cpp src/vmic_shm.cpp)
  set_target_properties(DustXMic PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION driver
    OUTPUT_NAME "DustXMic"
    PREFIX ""
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/vmic/macos/Info.plist"
  )
  target_link_libraries(DustXMic PRIVATE "-framework CoreAudio" "-framework CoreFoundation")

  add_library(DustXCamPlugin MODULE vcam/macos/dustx_dal.mm src/vcam_shm.cpp)
  set_target_properties(DustXCamPlugin PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    OUTPUT_NAME "DustXCam"
    PREFIX ""
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/vcam/macos/Info-dal.plist"
  )
  target_link_libraries(DustXCamPlugin PRIVATE
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreMediaIO"
    "-framework CoreVideo"
  )
  target_compile_options(DustXCamPlugin PRIVATE -fobjc-arc)

  add_executable(DustX MACOSX_BUNDLE ${DUSTX_COMMON} src/app_macos.mm src/vcam_install_macos.mm src/mesh_tun_macos.mm src/ssh_host.cpp src/ssh_host_posix.cpp)
  add_dependencies(DustX DustXMic)
  add_dependencies(DustX DustXCamPlugin)
  add_dependencies(DustX DustXCamera)
  set_target_properties(DustX PROPERTIES
    OUTPUT_NAME "尘埃X"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist"
    MACOSX_BUNDLE_BUNDLE_NAME "尘埃X"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.dustx.remotedesk"
  )
  target_link_libraries(DustX PRIVATE
    "-framework Cocoa"
    "-framework WebKit"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework Network"
  )
  target_compile_options(DustX PRIVATE -fobjc-arc)
  add_custom_command(TARGET DustX POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:DustX>/../Library/SystemExtensions"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "$<TARGET_FILE_DIR:DustXCamera>/../.."
      "$<TARGET_FILE_DIR:DustX>/../Library/SystemExtensions/com.dustx.remotedesk.camera.systemextension"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/web"
      "$<TARGET_FILE_DIR:DustX>/../Resources/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/../../src/remote/web"
      "$<TARGET_FILE_DIR:DustX>/../Resources/web/remote-ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "$<TARGET_FILE_DIR:DustXMic>/../.."
      "$<TARGET_FILE_DIR:DustX>/../Resources/DustXMic.driver"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "$<TARGET_FILE_DIR:DustXCamPlugin>/../.."
      "$<TARGET_FILE_DIR:DustX>/../Resources/DustXCam.plugin"
  )
  if(DEFINED DUSTX_PLATFORM_TOOLS)
    add_custom_command(TARGET DustX POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DustX>/../Resources/platform-tools"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${DUSTX_PLATFORM_TOOLS}/adb"
        "$<TARGET_FILE_DIR:DustX>/../Resources/platform-tools/adb"
      COMMAND chmod +x "$<TARGET_FILE_DIR:DustX>/../Resources/platform-tools/adb"
    )
  endif()
  add_custom_command(TARGET DustX POST_BUILD
    COMMAND /bin/zsh "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign_mac.sh"
      "$<TARGET_FILE_DIR:DustX>/../.."
      "${CMAKE_CURRENT_SOURCE_DIR}/vcam/macos/DustXCamera.entitlements"
      "${CMAKE_CURRENT_SOURCE_DIR}/macos/DustX.entitlements"
  )
  set_property(TARGET DustX APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/macos/DustX.entitlements"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcam/macos/DustXCamera.entitlements"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign_mac.sh"
  )
elseif(WIN32)
  add_compile_options(/utf-8)
  add_library(DustXCam SHARED
    vcam/windows/dustx_cam.cpp
    src/vcam_shm.cpp
  )
  target_sources(DustXCam PRIVATE vcam/windows/dustx_cam.def)
  target_compile_definitions(DustXCam PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(DustXCam PRIVATE strmiids ole32 oleaut32 uuid)

  add_library(DustXMic SHARED
    vmic/windows/dustx_mic.cpp
    src/vmic_shm.cpp
  )
  target_sources(DustXMic PRIVATE vmic/windows/dustx_mic.def)
  target_compile_definitions(DustXMic PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(DustXMic PRIVATE strmiids ole32 oleaut32 uuid)

  add_executable(DustX WIN32 ${DUSTX_COMMON} src/app_windows.cpp src/vcam_install_win.cpp src/mesh_tun_win.cpp src/ssh_host.cpp src/ssh_host_win.cpp "${CMAKE_CURRENT_BINARY_DIR}/dustx.rc")
  add_dependencies(DustX DustXCam DustXMic)
  set_target_properties(DustX PROPERTIES OUTPUT_NAME "DustX")
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/dustx.rc" PROPERTIES GENERATED TRUE)
  # .rc already embeds app.manifest; MSVC's default linker manifest would duplicate it.
  target_link_options(DustX PRIVATE "/MANIFEST:NO")
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Declare(
    webview2
    URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2849.39
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_GetProperties(webview2)
  if(NOT webview2_POPULATED)
    FetchContent_Populate(webview2)
  endif()
  target_include_directories(DustX PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WV2_LIB "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib")
  else()
    set(WV2_LIB "${webview2_SOURCE_DIR}/build/native/x86/WebView2LoaderStatic.lib")
  endif()
  target_compile_definitions(DustX PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN "DUSTX_APP_VERSION=\"${DUSTX_APP_VERSION}\"")
  target_link_libraries(DustX PRIVATE
    "${WV2_LIB}"
    ole32 oleaut32 uuid user32 gdi32 advapi32 shlwapi version strmiids iphlpapi ws2_32
  )
  FetchContent_Declare(
    wintun
    URL https://www.wintun.net/builds/wintun-0.14.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_GetProperties(wintun)
  if(NOT wintun_POPULATED)
    FetchContent_Populate(wintun)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(WINTUN_DLL "${wintun_SOURCE_DIR}/bin/arm64/wintun.dll")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WINTUN_DLL "${wintun_SOURCE_DIR}/bin/amd64/wintun.dll")
  else()
    set(WINTUN_DLL "${wintun_SOURCE_DIR}/bin/x86/wintun.dll")
  endif()
  FetchContent_Declare(
    win32openssh
    URL https://github.com/PowerShell/Win32-OpenSSH/releases/download/v9.5.0.0p1-Beta/OpenSSH-Win64.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_GetProperties(win32openssh)
  if(NOT win32openssh_POPULATED)
    FetchContent_Populate(win32openssh)
  endif()
  if(EXISTS "${win32openssh_SOURCE_DIR}/install-sshd.ps1")
    set(OPENSSH_DIR "${win32openssh_SOURCE_DIR}")
  elseif(EXISTS "${win32openssh_SOURCE_DIR}/OpenSSH-Win64/install-sshd.ps1")
    set(OPENSSH_DIR "${win32openssh_SOURCE_DIR}/OpenSSH-Win64")
  else()
    message(FATAL_ERROR "OpenSSH-Win64.zip 里没有 install-sshd.ps1")
  endif()
  add_custom_command(TARGET DustX POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/web"
      "$<TARGET_FILE_DIR:DustX>/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/../../src/remote/web"
      "$<TARGET_FILE_DIR:DustX>/web/remote-ui"
    COMMAND ${CMAKE_COMMAND} -E copy
      "$<TARGET_FILE:DustXCam>"
      "$<TARGET_FILE_DIR:DustX>/"
    COMMAND ${CMAKE_COMMAND} -E copy
      "$<TARGET_FILE:DustXMic>"
      "$<TARGET_FILE_DIR:DustX>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${WINTUN_DLL}"
      "$<TARGET_FILE_DIR:DustX>/wintun.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${OPENSSH_DIR}"
      "$<TARGET_FILE_DIR:DustX>/openssh"
  )
  if(DEFINED DUSTX_PLATFORM_TOOLS)
    add_custom_command(TARGET DustX POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:DustX>/platform-tools"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${DUSTX_PLATFORM_TOOLS}/adb.exe"
        "$<TARGET_FILE_DIR:DustX>/platform-tools/adb.exe"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${DUSTX_PLATFORM_TOOLS}/AdbWinApi.dll"
        "$<TARGET_FILE_DIR:DustX>/platform-tools/AdbWinApi.dll"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${DUSTX_PLATFORM_TOOLS}/AdbWinUsbApi.dll"
        "$<TARGET_FILE_DIR:DustX>/platform-tools/AdbWinUsbApi.dll"
    )
  endif()
else()
  message(FATAL_ERROR "尘埃X desktop client is macOS and Windows only")
endif()
