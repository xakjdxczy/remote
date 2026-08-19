#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOHardwarePlugIn.h>
#import <CoreMediaIO/CMIOSampleBuffer.h>
#import <CoreVideo/CoreVideo.h>

#include "../../src/vcam_shm.hpp"

#include <cstring>
#include <mach/mach_time.h>

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000ull
#endif

namespace {

constexpr int kW = dustx::kVcamWidth;
constexpr int kH = dustx::kVcamHeight;
constexpr Float64 kFps = 30.0;
constexpr UInt32 kVirt = 'virt';

enum Kind { kKindPlugin = 1, kKindDevice, kKindStream };

struct State {
  CMIOObjectID plugin = 0;
  CMIOObjectID device = 0;
  CMIOObjectID stream = 0;
  bool running = false;
  pid_t master = -1;
  bool exclude = false;
  CMSimpleQueueRef queue = nullptr;
  CFTypeRef clock = nullptr;
  CMIODeviceStreamQueueAlteredProc altered = nullptr;
  void* altered_ctx = nullptr;
  UInt64 seq = 0;
  uint64_t first_host = 0;
  dispatch_source_t timer = nullptr;
  dustx::VcamShm shm;
};

State g;
ULONG g_refs = 1;
CMIOHardwarePlugInInterface g_iface;
CMIOHardwarePlugInInterface* g_iface_ptr = &g_iface;
CMIOHardwarePlugInRef g_ref = &g_iface_ptr;

Kind kind_of(CMIOObjectID id) {
  if (id == g.plugin) return kKindPlugin;
  if (id == g.device) return kKindDevice;
  if (id == g.stream) return kKindStream;
  return static_cast<Kind>(0);
}

void fill_bars(uint8_t* bgra, size_t stride, uint32_t tick) {
  static const uint8_t bars[7][3] = {
      {192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0},
      {192, 0, 192},   {192, 0, 0},   {0, 0, 192},
  };
  for (int y = 0; y < kH; ++y) {
    uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = 0; x < kW; ++x) {
      const int b = (x * 7) / kW;
      row[x * 4 + 0] = bars[b][2];
      row[x * 4 + 1] = bars[b][1];
      row[x * 4 + 2] = bars[b][0];
      row[x * 4 + 3] = 255;
    }
  }
  const int stripe = static_cast<int>((tick / 2) % static_cast<uint32_t>(kW));
  for (int y = kH / 2 - 8; y < kH / 2 + 8; ++y) {
    uint8_t* p = bgra + static_cast<size_t>(y) * stride + static_cast<size_t>(stripe) * 4;
    p[0] = 255;
    p[1] = 255;
    p[2] = 255;
  }
}

void rgb_to_bgra(const uint8_t* rgb, uint8_t* bgra, size_t stride) {
  for (int y = 0; y < kH; ++y) {
    uint8_t* dst = bgra + static_cast<size_t>(y) * stride;
    const uint8_t* src = rgb + static_cast<size_t>(y) * kW * 3;
    for (int x = 0; x < kW; ++x) {
      dst[x * 4 + 0] = src[x * 3 + 2];
      dst[x * 4 + 1] = src[x * 3 + 1];
      dst[x * 4 + 2] = src[x * 3 + 0];
      dst[x * 4 + 3] = 255;
    }
  }
}

CMVideoFormatDescriptionRef make_format() {
  CMVideoFormatDescriptionRef desc = nullptr;
  CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCVPixelFormatType_32BGRA, kW, kH, nullptr, &desc);
  return desc;
}

void emit_frame() {
  if (!g.queue || !g.running) return;
  if (CMSimpleQueueGetFullness(g.queue) >= 1.0) return;

  CVPixelBufferRef px = nullptr;
  NSDictionary* attrs = @{
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  if (CVPixelBufferCreate(kCFAllocatorDefault, kW, kH, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs,
                          &px) != kCVReturnSuccess) {
    return;
  }
  CVPixelBufferLockBaseAddress(px, 0);
  auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(px));
  const size_t stride = CVPixelBufferGetBytesPerRow(px);
  uint8_t rgb[dustx::kVcamMaxBytes];
  int w = kW, h = kH;
  uint32_t fid = 0;
  if (!g.shm.valid()) g.shm.open_reader();
  if (g.shm.read_rgb(rgb, &w, &h, &fid)) {
    rgb_to_bgra(rgb, dst, stride);
  } else {
    fill_bars(dst, stride, static_cast<uint32_t>(g.seq));
  }
  CVPixelBufferUnlockBaseAddress(px, 0);

  if (g.first_host == 0) g.first_host = mach_absolute_time();
  const CMTimeScale scale = static_cast<CMTimeScale>(kFps * 100);
  CMTime duration = CMTimeMake(scale / static_cast<CMTimeScale>(kFps), scale);
  CMTime pts = CMTimeMake(duration.value * static_cast<int64_t>(g.seq), scale);
  CMSampleTimingInfo timing{};
  timing.duration = duration;
  timing.presentationTimeStamp = pts;
  timing.decodeTimeStamp = pts;
  if (g.clock) CMIOStreamClockPostTimingEvent(pts, mach_absolute_time(), true, g.clock);

  CMFormatDescriptionRef format = nullptr;
  CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, px, &format);
  g.seq = CMIOGetNextSequenceNumber(g.seq);
  CMSampleBufferRef sample = nullptr;
  CMIOSampleBufferCreateForImageBuffer(kCFAllocatorDefault, px, format, &timing, g.seq, kCMIOSampleBufferNoDiscontinuities,
                                       &sample);
  if (format) CFRelease(format);
  CVPixelBufferRelease(px);
  if (!sample) return;
  CMSimpleQueueEnqueue(g.queue, sample);
  if (g.altered) g.altered(g.stream, sample, g.altered_ctx);
}

void start_timer() {
  if (g.timer) {
    dispatch_resume(g.timer);
    return;
  }
  g.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                   dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
  dispatch_source_set_timer(g.timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 30, NSEC_PER_SEC / 120);
  dispatch_source_set_event_handler(g.timer, ^{ emit_frame(); });
  dispatch_resume(g.timer);
}

void stop_timer() {
  if (g.timer) dispatch_suspend(g.timer);
  g.first_host = 0;
}

ULONG AddRef(void*) { return ++g_refs; }
ULONG Release(void*) { return g_refs > 0 ? --g_refs : 0; }

HRESULT QueryInterface(void*, REFIID iid, LPVOID* out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, iid);
  const bool ok = CFEqual(uuid, IUnknownUUID) || CFEqual(uuid, kCMIOHardwarePlugInInterfaceID);
  CFRelease(uuid);
  if (!ok) return E_NOINTERFACE;
  ++g_refs;
  *out = g_ref;
  return kCMIOHardwareNoError;
}

OSStatus Initialize(CMIOHardwarePlugInRef) { return kCMIOHardwareUnspecifiedError; }

OSStatus InitializeWithObjectID(CMIOHardwarePlugInRef, CMIOObjectID objectID) {
  g.plugin = objectID;
  OSStatus err = CMIOObjectCreate(g_ref, kCMIOObjectSystemObject, kCMIODeviceClassID, &g.device);
  if (err != noErr) return err;
  err = CMIOObjectCreate(g_ref, g.device, kCMIOStreamClassID, &g.stream);
  if (err != noErr) return err;
  err = CMIOObjectsPublishedAndDied(g_ref, kCMIOObjectSystemObject, 1, &g.device, 0, 0);
  if (err != kCMIOHardwareNoError) return err;
  err = CMIOObjectsPublishedAndDied(g_ref, g.device, 1, &g.stream, 0, 0);
  g.shm.open_reader();
  return err;
}

OSStatus Teardown(CMIOHardwarePlugInRef) {
  if (g.timer) {
    dispatch_source_cancel(g.timer);
    g.timer = nullptr;
  }
  if (g.clock) {
    CMIOStreamClockInvalidate(g.clock);
    CFRelease(g.clock);
    g.clock = nullptr;
  }
  if (g.queue) {
    CFRelease(g.queue);
    g.queue = nullptr;
  }
  g.shm.close();
  return kCMIOHardwareNoError;
}

void ObjectShow(CMIOHardwarePlugInRef, CMIOObjectID) {}

Boolean ObjectHasProperty(CMIOHardwarePlugInRef, CMIOObjectID objectID, const CMIOObjectPropertyAddress* address) {
  const Kind k = kind_of(objectID);
  switch (address->mSelector) {
    case kCMIOObjectPropertyName:
    case kCMIOObjectPropertyManufacturer:
      return true;
    case kCMIODevicePropertyPlugIn:
    case kCMIODevicePropertyDeviceUID:
    case kCMIODevicePropertyModelUID:
    case kCMIODevicePropertyTransportType:
    case kCMIODevicePropertyDeviceIsAlive:
    case kCMIODevicePropertyDeviceHasChanged:
    case kCMIODevicePropertyDeviceIsRunning:
    case kCMIODevicePropertyDeviceIsRunningSomewhere:
    case kCMIODevicePropertyDeviceCanBeDefaultDevice:
    case kCMIODevicePropertyHogMode:
    case kCMIODevicePropertyLatency:
    case kCMIODevicePropertyStreams:
    case kCMIODevicePropertyExcludeNonDALAccess:
    case kCMIODevicePropertyCanProcessAVCCommand:
    case kCMIODevicePropertyCanProcessRS422Command:
    case kCMIODevicePropertyDeviceControl:
      return k == kKindDevice || (k == kKindStream && address->mSelector == kCMIODevicePropertyLatency);
    case kCMIOStreamPropertyDirection:
    case kCMIOStreamPropertyTerminalType:
    case kCMIOStreamPropertyStartingChannel:
    case kCMIOStreamPropertyFormatDescriptions:
    case kCMIOStreamPropertyFormatDescription:
    case kCMIOStreamPropertyFrameRateRanges:
    case kCMIOStreamPropertyFrameRate:
    case kCMIOStreamPropertyFrameRates:
    case kCMIOStreamPropertyMinimumFrameRate:
    case kCMIOStreamPropertyClock:
      return k == kKindStream;
    default:
      return false;
  }
}

OSStatus ObjectIsPropertySettable(CMIOHardwarePlugInRef, CMIOObjectID, const CMIOObjectPropertyAddress* address,
                                  Boolean* settable) {
  *settable = (address->mSelector == kCMIODevicePropertyExcludeNonDALAccess ||
               address->mSelector == kCMIODevicePropertyDeviceControl);
  return kCMIOHardwareNoError;
}

OSStatus ObjectGetPropertyDataSize(CMIOHardwarePlugInRef, CMIOObjectID, const CMIOObjectPropertyAddress* address, UInt32,
                                   const void*, UInt32* size) {
  switch (address->mSelector) {
    case kCMIOObjectPropertyName:
    case kCMIOObjectPropertyManufacturer:
    case kCMIODevicePropertyDeviceUID:
    case kCMIODevicePropertyModelUID:
    case kCMIOStreamPropertyFormatDescriptions:
    case kCMIOStreamPropertyFormatDescription:
    case kCMIOStreamPropertyClock:
      *size = sizeof(CFTypeRef);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyPlugIn:
    case kCMIODevicePropertyTransportType:
    case kCMIODevicePropertyDeviceIsAlive:
    case kCMIODevicePropertyDeviceHasChanged:
    case kCMIODevicePropertyDeviceIsRunning:
    case kCMIODevicePropertyDeviceIsRunningSomewhere:
    case kCMIODevicePropertyDeviceCanBeDefaultDevice:
    case kCMIODevicePropertyLatency:
    case kCMIODevicePropertyExcludeNonDALAccess:
    case kCMIOStreamPropertyDirection:
    case kCMIOStreamPropertyTerminalType:
    case kCMIOStreamPropertyStartingChannel:
      *size = sizeof(UInt32);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyHogMode:
    case kCMIODevicePropertyDeviceControl:
      *size = sizeof(pid_t);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyStreams:
      *size = sizeof(CMIOStreamID);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyCanProcessAVCCommand:
    case kCMIODevicePropertyCanProcessRS422Command:
      *size = sizeof(Boolean);
      return kCMIOHardwareNoError;
    case kCMIOStreamPropertyFrameRateRanges:
      *size = sizeof(AudioValueRange);
      return kCMIOHardwareNoError;
    case kCMIOStreamPropertyFrameRate:
    case kCMIOStreamPropertyFrameRates:
    case kCMIOStreamPropertyMinimumFrameRate:
      *size = sizeof(Float64);
      return kCMIOHardwareNoError;
    default:
      *size = 0;
      return kCMIOHardwareUnknownPropertyError;
  }
}

OSStatus ObjectGetPropertyData(CMIOHardwarePlugInRef, CMIOObjectID objectID, const CMIOObjectPropertyAddress* address,
                               UInt32, const void*, UInt32, UInt32* used, void* data) {
  const Kind k = kind_of(objectID);
  auto set_str = [&](CFStringRef s) {
    *static_cast<CFStringRef*>(data) = s;
    *used = sizeof(CFStringRef);
  };
  auto set_u32 = [&](UInt32 v) {
    *static_cast<UInt32*>(data) = v;
    *used = sizeof(UInt32);
  };
  switch (address->mSelector) {
    case kCMIOObjectPropertyName:
      set_str(k == kKindStream ? CFSTR("尘埃X") : CFSTR("尘埃X 摄像头"));
      return kCMIOHardwareNoError;
    case kCMIOObjectPropertyManufacturer:
      set_str(CFSTR("尘埃X"));
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyPlugIn:
      *static_cast<CMIOObjectID*>(data) = g.plugin;
      *used = sizeof(CMIOObjectID);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyDeviceUID:
      set_str(CFSTR("com.dustx.remotedesk.camera"));
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyModelUID:
      set_str(CFSTR("com.dustx.remotedesk.camera.model"));
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyTransportType:
      set_u32(kVirt);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyDeviceIsAlive:
    case kCMIODevicePropertyDeviceIsRunning:
    case kCMIODevicePropertyDeviceIsRunningSomewhere:
    case kCMIODevicePropertyDeviceCanBeDefaultDevice:
      set_u32(1);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyDeviceHasChanged:
    case kCMIODevicePropertyLatency:
    case kCMIOStreamPropertyStartingChannel:
      set_u32(0);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyHogMode:
      *static_cast<pid_t*>(data) = -1;
      *used = sizeof(pid_t);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyStreams:
      *static_cast<CMIOStreamID*>(data) = g.stream;
      *used = sizeof(CMIOStreamID);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyExcludeNonDALAccess:
      set_u32(g.exclude ? 1 : 0);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyCanProcessAVCCommand:
    case kCMIODevicePropertyCanProcessRS422Command:
      *static_cast<Boolean*>(data) = false;
      *used = sizeof(Boolean);
      return kCMIOHardwareNoError;
    case kCMIODevicePropertyDeviceControl:
      *static_cast<pid_t*>(data) = g.master;
      *used = sizeof(pid_t);
      return kCMIOHardwareNoError;
    case kCMIOStreamPropertyDirection:
      set_u32(0);
      return kCMIOHardwareNoError;
    case kCMIOStreamPropertyTerminalType:
      set_u32(0x0201);
      return kCMIOHardwareNoError;
    case kCMIOStreamPropertyFormatDescriptions: {
      CMVideoFormatDescriptionRef desc = make_format();
      *static_cast<CFArrayRef*>(data) = CFArrayCreate(kCFAllocatorDefault, (const void**)&desc, 1, &kCFTypeArrayCallBacks);
      if (desc) CFRelease(desc);
      *used = sizeof(CFArrayRef);
      return kCMIOHardwareNoError;
    }
    case kCMIOStreamPropertyFormatDescription:
      *static_cast<CMVideoFormatDescriptionRef*>(data) = make_format();
      *used = sizeof(CMVideoFormatDescriptionRef);
      return kCMIOHardwareNoError;
    case kCMIOStreamPropertyFrameRateRanges: {
      AudioValueRange r{kFps, kFps};
      *static_cast<AudioValueRange*>(data) = r;
      *used = sizeof(AudioValueRange);
      return kCMIOHardwareNoError;
    }
    case kCMIOStreamPropertyFrameRate:
    case kCMIOStreamPropertyFrameRates:
    case kCMIOStreamPropertyMinimumFrameRate:
      *static_cast<Float64*>(data) = kFps;
      *used = sizeof(Float64);
      return kCMIOHardwareNoError;
    case kCMIOStreamPropertyClock:
      if (!g.clock) {
        CMIOStreamClockCreate(kCFAllocatorDefault, CFSTR("dustx-cam-clock"), &g, CMTimeMake(1, 10), 100, 10, &g.clock);
      }
      *static_cast<CFTypeRef*>(data) = g.clock;
      if (g.clock) CFRetain(g.clock);
      *used = sizeof(CFTypeRef);
      return kCMIOHardwareNoError;
    default:
      *used = 0;
      return kCMIOHardwareUnknownPropertyError;
  }
}

OSStatus ObjectSetPropertyData(CMIOHardwarePlugInRef, CMIOObjectID, const CMIOObjectPropertyAddress* address, UInt32,
                               const void*, UInt32, const void* data) {
  if (address->mSelector == kCMIODevicePropertyExcludeNonDALAccess) {
    g.exclude = *static_cast<const UInt32*>(data) != 0;
    return kCMIOHardwareNoError;
  }
  if (address->mSelector == kCMIODevicePropertyDeviceControl) {
    g.master = *static_cast<const pid_t*>(data);
    return kCMIOHardwareNoError;
  }
  return kCMIOHardwareUnknownPropertyError;
}

OSStatus DeviceSuspend(CMIOHardwarePlugInRef, CMIODeviceID) { return kCMIOHardwareNoError; }
OSStatus DeviceResume(CMIOHardwarePlugInRef, CMIODeviceID) { return kCMIOHardwareNoError; }

OSStatus DeviceStartStream(CMIOHardwarePlugInRef, CMIODeviceID, CMIOStreamID) {
  g.running = true;
  start_timer();
  return kCMIOHardwareNoError;
}

OSStatus DeviceStopStream(CMIOHardwarePlugInRef, CMIODeviceID, CMIOStreamID) {
  g.running = false;
  stop_timer();
  return kCMIOHardwareNoError;
}

OSStatus DeviceProcessAVCCommand(CMIOHardwarePlugInRef, CMIODeviceID, CMIODeviceAVCCommand*) {
  return kCMIOHardwareIllegalOperationError;
}
OSStatus DeviceProcessRS422Command(CMIOHardwarePlugInRef, CMIODeviceID, CMIODeviceRS422Command*) {
  return kCMIOHardwareIllegalOperationError;
}

OSStatus StreamCopyBufferQueue(CMIOHardwarePlugInRef, CMIOStreamID, CMIODeviceStreamQueueAlteredProc proc, void* ctx,
                               CMSimpleQueueRef* queue) {
  if (!g.queue) CMSimpleQueueCreate(kCFAllocatorDefault, static_cast<int32_t>(kFps), &g.queue);
  if (!g.clock) {
    CMIOStreamClockCreate(kCFAllocatorDefault, CFSTR("dustx-cam-clock"), &g, CMTimeMake(1, 10), 100, 10, &g.clock);
  }
  g.altered = proc;
  g.altered_ctx = ctx;
  if (g.queue) CFRetain(g.queue);
  *queue = g.queue;
  return kCMIOHardwareNoError;
}

OSStatus StreamDeckPlay(CMIOHardwarePlugInRef, CMIOStreamID) { return kCMIOHardwareIllegalOperationError; }
OSStatus StreamDeckStop(CMIOHardwarePlugInRef, CMIOStreamID) { return kCMIOHardwareIllegalOperationError; }
OSStatus StreamDeckJog(CMIOHardwarePlugInRef, CMIOStreamID, SInt32) { return kCMIOHardwareIllegalOperationError; }
OSStatus StreamDeckCueTo(CMIOHardwarePlugInRef, CMIOStreamID, Float64, Boolean) {
  return kCMIOHardwareIllegalOperationError;
}

void fill_iface() {
  std::memset(&g_iface, 0, sizeof(g_iface));
  g_iface.QueryInterface = QueryInterface;
  g_iface.AddRef = AddRef;
  g_iface.Release = Release;
  g_iface.Initialize = Initialize;
  g_iface.InitializeWithObjectID = InitializeWithObjectID;
  g_iface.Teardown = Teardown;
  g_iface.ObjectShow = ObjectShow;
  g_iface.ObjectHasProperty = ObjectHasProperty;
  g_iface.ObjectIsPropertySettable = ObjectIsPropertySettable;
  g_iface.ObjectGetPropertyDataSize = ObjectGetPropertyDataSize;
  g_iface.ObjectGetPropertyData = ObjectGetPropertyData;
  g_iface.ObjectSetPropertyData = ObjectSetPropertyData;
  g_iface.DeviceSuspend = DeviceSuspend;
  g_iface.DeviceResume = DeviceResume;
  g_iface.DeviceStartStream = DeviceStartStream;
  g_iface.DeviceStopStream = DeviceStopStream;
  g_iface.DeviceProcessAVCCommand = DeviceProcessAVCCommand;
  g_iface.DeviceProcessRS422Command = DeviceProcessRS422Command;
  g_iface.StreamCopyBufferQueue = StreamCopyBufferQueue;
  g_iface.StreamDeckPlay = StreamDeckPlay;
  g_iface.StreamDeckStop = StreamDeckStop;
  g_iface.StreamDeckJog = StreamDeckJog;
  g_iface.StreamDeckCueTo = StreamDeckCueTo;
}

}  // namespace

extern "C" void* PlugInMain(CFAllocatorRef, CFUUIDRef typeUUID) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{ fill_iface(); });
  if (typeUUID && CFEqual(typeUUID, kCMIOHardwarePlugInTypeID)) {
    ++g_refs;
    return g_ref;
  }
  return nullptr;
}
