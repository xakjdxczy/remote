#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOExtension.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include "../../src/vcam_shm.hpp"

#include <cstring>
#include <mach/mach_time.h>
#include <time.h>

static const int kW = dustx::kVcamWidth;
static const int kH = dustx::kVcamHeight;

@interface DustXStreamSource : NSObject <CMIOExtensionStreamSource>
@property(atomic, readonly) NSArray<CMIOExtensionStreamFormat*>* formats;
@property(nonatomic, strong) CMIOExtensionStream* stream;
@property(nonatomic, strong) dispatch_source_t timer;
@property(nonatomic, assign) BOOL running;
@end

@implementation DustXStreamSource {
  dustx::VcamShm _shm;
  uint32_t _last_id;
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  CMVideoFormatDescriptionRef desc = nullptr;
  CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCVPixelFormatType_32BGRA, kW, kH, nullptr, &desc);
  CMIOExtensionStreamFormat* fmt =
      [CMIOExtensionStreamFormat streamFormatWithFormatDescription:desc
                                                  maxFrameDuration:CMTimeMake(1, 30)
                                                  minFrameDuration:CMTimeMake(1, 30)
                                               validFrameDurations:nil];
  if (desc) CFRelease(desc);
  _formats = @[ fmt ];
  _shm.open_reader();
  return self;
}

- (NSSet<CMIOExtensionProperty>*)availableProperties {
  return [NSSet set];
}

- (CMIOExtensionStreamProperties*)streamPropertiesForProperties:(NSSet<CMIOExtensionProperty>*)properties
                                                          error:(NSError**)outError {
  (void)properties;
  (void)outError;
  return [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties*)streamProperties error:(NSError**)outError {
  (void)streamProperties;
  (void)outError;
  return YES;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient*)client {
  (void)client;
  return YES;
}

- (BOOL)startStreamAndReturnError:(NSError**)outError {
  (void)outError;
  if (self.running) return YES;
  if (!_shm.valid()) _shm.open_reader();
  self.running = YES;
  dispatch_queue_t q = dispatch_queue_create("com.dustx.vcam.timer", DISPATCH_QUEUE_SERIAL);
  self.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
  dispatch_source_set_timer(self.timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 30, NSEC_PER_SEC / 120);
  __weak DustXStreamSource* weak = self;
  dispatch_source_set_event_handler(self.timer, ^{ [weak emit]; });
  dispatch_resume(self.timer);
  return YES;
}

- (BOOL)stopStreamAndReturnError:(NSError**)outError {
  (void)outError;
  self.running = NO;
  if (self.timer) {
    dispatch_source_cancel(self.timer);
    self.timer = nil;
  }
  return YES;
}

- (void)emit {
  if (!self.running || !self.stream) return;
  uint8_t rgb[dustx::kVcamMaxBytes];
  uint32_t fid = 0;
  int w = kW, h = kH;
  if (!_shm.read_rgb(rgb, &w, &h, &fid)) {
    std::memset(rgb, 16, sizeof(rgb));
  }
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
  size_t stride = CVPixelBufferGetBytesPerRow(px);
  for (int y = 0; y < kH; ++y) {
    uint8_t* row = dst + y * stride;
    const uint8_t* src = rgb + y * kW * 3;
    for (int x = 0; x < kW; ++x) {
      row[x * 4 + 0] = src[x * 3 + 2];
      row[x * 4 + 1] = src[x * 3 + 1];
      row[x * 4 + 2] = src[x * 3 + 0];
      row[x * 4 + 3] = 255;
    }
  }
  CVPixelBufferUnlockBaseAddress(px, 0);
  CMVideoFormatDescriptionRef desc = nullptr;
  CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, px, &desc);
  CMSampleTimingInfo timing{};
  timing.duration = CMTimeMake(1, 30);
  timing.presentationTimeStamp = CMTimeMake(static_cast<int64_t>(fid ? fid : mach_absolute_time()), 30);
  timing.decodeTimeStamp = kCMTimeInvalid;
  CMSampleBufferRef sample = nullptr;
  CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, px, true, nullptr, nullptr, desc, &timing, &sample);
  if (sample) {
    uint64_t host = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    [self.stream sendSampleBuffer:sample discontinuity:CMIOExtensionStreamDiscontinuityFlagNone hostTimeInNanoseconds:host];
    CFRelease(sample);
  }
  if (desc) CFRelease(desc);
  CVPixelBufferRelease(px);
}

@end

@interface DustXDeviceSource : NSObject <CMIOExtensionDeviceSource>
@property(nonatomic, strong) DustXStreamSource* streamSource;
@property(nonatomic, strong) CMIOExtensionStream* stream;
@end

@implementation DustXDeviceSource
- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _streamSource = [DustXStreamSource new];
  _stream = [CMIOExtensionStream streamWithLocalizedName:@"尘埃"
                                                 streamID:[[NSUUID alloc] initWithUUIDString:@"A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F71"]
                                                direction:CMIOExtensionStreamDirectionSource
                                                clockType:CMIOExtensionStreamClockTypeHostTime
                                                   source:_streamSource];
  _streamSource.stream = _stream;
  return self;
}

- (NSSet<CMIOExtensionProperty>*)availableProperties {
  return [NSSet setWithObject:CMIOExtensionPropertyDeviceModel];
}

- (CMIOExtensionDeviceProperties*)devicePropertiesForProperties:(NSSet<CMIOExtensionProperty>*)properties
                                                          error:(NSError**)outError {
  (void)outError;
  CMIOExtensionDeviceProperties* props = [CMIOExtensionDeviceProperties devicePropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyDeviceModel]) props.model = @"尘埃 摄像头";
  return props;
}

- (BOOL)setDeviceProperties:(CMIOExtensionDeviceProperties*)deviceProperties error:(NSError**)outError {
  (void)deviceProperties;
  (void)outError;
  return YES;
}
@end

@interface DustXProviderSource : NSObject <CMIOExtensionProviderSource>
@property(nonatomic, strong) CMIOExtensionProvider* provider;
@property(nonatomic, strong) DustXDeviceSource* deviceSource;
@property(nonatomic, strong) CMIOExtensionDevice* device;
@end

@implementation DustXProviderSource
- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _provider = [[CMIOExtensionProvider alloc] initWithSource:self clientQueue:nil];
  _deviceSource = [DustXDeviceSource new];
  _device = [CMIOExtensionDevice deviceWithLocalizedName:@"尘埃 摄像头"
                                                deviceID:[[NSUUID alloc] initWithUUIDString:@"A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F70"]
                                          legacyDeviceID:@"dustx-camera"
                                                  source:_deviceSource];
  [_device addStream:_deviceSource.stream error:nil];
  [_provider addDevice:_device error:nil];
  return self;
}

- (BOOL)connectClient:(CMIOExtensionClient*)client error:(NSError**)outError {
  (void)client;
  (void)outError;
  return YES;
}

- (void)disconnectClient:(CMIOExtensionClient*)client {
  (void)client;
}

- (NSSet<CMIOExtensionProperty>*)availableProperties {
  return [NSSet setWithObject:CMIOExtensionPropertyProviderManufacturer];
}

- (CMIOExtensionProviderProperties*)providerPropertiesForProperties:(NSSet<CMIOExtensionProperty>*)properties
                                                              error:(NSError**)outError {
  (void)outError;
  CMIOExtensionProviderProperties* props = [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer]) props.manufacturer = @"尘埃";
  return props;
}

- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties*)providerProperties error:(NSError**)outError {
  (void)providerProperties;
  (void)outError;
  return YES;
}
@end

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    DustXProviderSource* src = [DustXProviderSource new];
    [CMIOExtensionProvider startServiceWithProvider:src.provider];
    [[NSRunLoop mainRunLoop] run];
  }
  return 0;
}
