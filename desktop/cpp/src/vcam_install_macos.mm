#import <Foundation/Foundation.h>

#include "vcam_install.hpp"

#include <string>

namespace {

NSString* resource_bundle(NSString* name, NSString* ext) {
  NSString* path = [[NSBundle mainBundle] pathForResource:name ofType:ext];
  if (path) return path;
  return [[NSBundle mainBundle].resourcePath stringByAppendingPathComponent:[name stringByAppendingPathExtension:ext]];
}

bool exists(NSString* path) {
  return path && [[NSFileManager defaultManager] fileExistsAtPath:path];
}

void copy_plugins_async(NSString* cam_src, NSString* mic_src, bool need_cam, bool need_mic) {
  if (!need_cam && !need_mic) return;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSMutableArray<NSString*>* cmds = [NSMutableArray array];
    if (need_cam && cam_src) {
      [cmds addObject:@"mkdir -p /Library/CoreMediaIO/Plug-Ins/DAL"];
      [cmds addObject:@"rm -rf /Library/CoreMediaIO/Plug-Ins/DAL/DustXCam.plugin"];
      [cmds addObject:[NSString stringWithFormat:@"cp -R '%@' /Library/CoreMediaIO/Plug-Ins/DAL/DustXCam.plugin", cam_src]];
      [cmds addObject:@"chmod -R 755 /Library/CoreMediaIO/Plug-Ins/DAL/DustXCam.plugin"];
    }
    if (need_mic && mic_src) {
      [cmds addObject:@"mkdir -p /Library/Audio/Plug-Ins/HAL"];
      [cmds addObject:@"rm -rf /Library/Audio/Plug-Ins/HAL/DustXMic.driver"];
      [cmds addObject:[NSString stringWithFormat:@"cp -R '%@' /Library/Audio/Plug-Ins/HAL/DustXMic.driver", mic_src]];
      [cmds addObject:@"chmod -R 755 /Library/Audio/Plug-Ins/HAL/DustXMic.driver"];
      [cmds addObject:@"killall coreaudiod"];
    }
    if (need_cam) {
      [cmds addObject:@"killall VDCAssistant AppleCameraAssistant 2>/dev/null; true"];
    }
    NSString* script = [NSString stringWithFormat:@"do shell script \"%@\" with administrator privileges",
                                                  [cmds componentsJoinedByString:@" && "]];
    NSAppleScript* as = [[NSAppleScript alloc] initWithSource:script];
    NSDictionary* err = nil;
    [as executeAndReturnError:&err];
    if (err) {
      NSLog(@"DustX plugin install: %@", err);
    } else {
      NSLog(@"DustX plugin install finished");
    }
  });
}

}  // namespace

namespace dustx {

bool install_vcam(std::string* message) {
  if (exists(@"/Library/CoreMediaIO/Plug-Ins/DAL/DustXCam.plugin")) {
    if (message) *message = "尘埃X 摄像头已安装。OBS / 会议软件请选「尘埃X 摄像头」。";
    return true;
  }
  NSString* cam = resource_bundle(@"DustXCam", @"plugin");
  if (!exists(cam)) {
    if (message) *message = "找不到 DustXCam.plugin，虚拟摄像头没有打进应用。";
    return false;
  }
  NSString* mic = resource_bundle(@"DustXMic", @"driver");
  copy_plugins_async(cam, exists(mic) ? mic : nil, true, !exists(@"/Library/Audio/Plug-Ins/HAL/DustXMic.driver") && exists(mic));
  if (message) {
    *message = "正在请求管理员权限安装「尘埃X 摄像头」。允许后重新打开 OBS，选视频采集设备。";
  }
  return true;
}

bool install_vmic(std::string* message) {
  if (exists(@"/Library/Audio/Plug-Ins/HAL/DustXMic.driver")) {
    if (message) *message = "尘埃X 麦克风已安装。会议软件请选「尘埃X 麦克风」。";
    return true;
  }
  NSString* mic = resource_bundle(@"DustXMic", @"driver");
  if (!exists(mic)) {
    if (message) *message = "找不到 DustXMic.driver，虚拟麦克风没有打进应用。";
    return false;
  }
  NSString* cam = resource_bundle(@"DustXCam", @"plugin");
  copy_plugins_async(cam, mic, exists(cam) && !exists(@"/Library/CoreMediaIO/Plug-Ins/DAL/DustXCam.plugin"), true);
  if (message) *message = "正在请求管理员权限安装「尘埃X 麦克风」。";
  return true;
}

}  // namespace dustx
