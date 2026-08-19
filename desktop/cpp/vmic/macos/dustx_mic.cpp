#include "../../src/vmic_shm.hpp"

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000ull
#endif

namespace {

enum {
  kPlugIn = kAudioObjectPlugInObject,
  kBox = 2,
  kDevice = 3,
  kStreamIn = 4,
};

const Float64 kRate = 48000;
const UInt32 kCh = 1;
const CFStringRef kName = CFSTR("尘埃X 麦克风");
const CFStringRef kUid = CFSTR("com.dustx.remotedesk.mic");
const CFStringRef kBoxUid = CFSTR("com.dustx.remotedesk.mic.box");
const CFStringRef kStreamUid = CFSTR("com.dustx.remotedesk.mic.input");

struct Driver {
  AudioServerPlugInDriverInterface* vtable;
  AudioServerPlugInDriverInterface iface;
  CFUUIDRef factory_uuid = nullptr;
  AudioServerPlugInHostRef host = nullptr;
  ULONG refs = 1;
  bool box_acquired = true;
  UInt32 io_count = 0;
  Float64 anchor_host = 0;
  UInt64 anchor_sample = 0;
  dustx::VmicShm shm;
  std::mutex mu;
};

Driver* as_driver(AudioServerPlugInDriverRef ref) {
  if (!ref || !*ref) return nullptr;
  return reinterpret_cast<Driver*>(reinterpret_cast<char*>(*ref) - offsetof(Driver, iface));
}

HRESULT drv_query(void* in, REFIID iid, LPVOID* out) {
  if (!in || !out) return E_POINTER;
  *out = nullptr;
  CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(nullptr, iid);
  bool ok = CFEqual(uuid, IUnknownUUID) || CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID);
  CFRelease(uuid);
  if (!ok) return E_NOINTERFACE;
  auto* d = as_driver(static_cast<AudioServerPlugInDriverRef>(in));
  if (!d) return E_FAIL;
  d->refs++;
  *out = in;
  return S_OK;
}

ULONG drv_add(void* in) {
  auto* d = as_driver(static_cast<AudioServerPlugInDriverRef>(in));
  return d ? ++d->refs : 0;
}

ULONG drv_rel(void* in) {
  auto* d = as_driver(static_cast<AudioServerPlugInDriverRef>(in));
  if (!d) return 0;
  ULONG n = --d->refs;
  if (n == 0) {
    if (d->factory_uuid) CFRelease(d->factory_uuid);
    delete d;
  }
  return n;
}

OSStatus initialize(AudioServerPlugInDriverRef ref, AudioServerPlugInHostRef host) {
  auto* d = as_driver(ref);
  if (!d) return kAudioHardwareBadObjectError;
  d->host = host;
  d->shm.open_reader();
  return kAudioHardwareNoError;
}

OSStatus create_device(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) {
  return kAudioHardwareUnsupportedOperationError;
}
OSStatus destroy_device(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }

OSStatus add_client(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) {
  return kAudioHardwareNoError;
}
OSStatus remove_client(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) {
  return kAudioHardwareNoError;
}

Boolean has_prop(AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* addr) {
  if (!addr) return false;
  const auto sel = addr->mSelector;
  if (obj == kPlugIn) {
    return sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass ||
           sel == kAudioObjectPropertyOwner || sel == kAudioObjectPropertyOwnedObjects ||
           sel == kAudioObjectPropertyManufacturer || sel == kAudioPlugInPropertyBoxList ||
           sel == kAudioPlugInPropertyTranslateUIDToBox || sel == kAudioPlugInPropertyDeviceList ||
           sel == kAudioPlugInPropertyTranslateUIDToDevice || sel == kAudioPlugInPropertyResourceBundle;
  }
  if (obj == kBox) {
    return sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass ||
           sel == kAudioObjectPropertyOwner || sel == kAudioObjectPropertyName ||
           sel == kAudioObjectPropertyManufacturer || sel == kAudioObjectPropertyOwnedObjects ||
           sel == kAudioBoxPropertyBoxUID || sel == kAudioBoxPropertyTransportType ||
           sel == kAudioBoxPropertyHasAudio || sel == kAudioBoxPropertyHasVideo ||
           sel == kAudioBoxPropertyHasMIDI || sel == kAudioBoxPropertyIsProtected ||
           sel == kAudioBoxPropertyAcquired || sel == kAudioBoxPropertyAcquisitionFailed ||
           sel == kAudioBoxPropertyDeviceList;
  }
  if (obj == kDevice) {
    return sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass ||
           sel == kAudioObjectPropertyOwner || sel == kAudioObjectPropertyName ||
           sel == kAudioObjectPropertyManufacturer || sel == kAudioObjectPropertyOwnedObjects ||
           sel == kAudioDevicePropertyDeviceUID || sel == kAudioDevicePropertyModelUID ||
           sel == kAudioDevicePropertyTransportType || sel == kAudioDevicePropertyRelatedDevices ||
           sel == kAudioDevicePropertyClockDomain || sel == kAudioDevicePropertyDeviceIsAlive ||
           sel == kAudioDevicePropertyDeviceIsRunning || sel == kAudioDevicePropertyDeviceCanBeDefaultDevice ||
           sel == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice || sel == kAudioDevicePropertyLatency ||
           sel == kAudioDevicePropertyStreams || sel == kAudioObjectPropertyControlList ||
           sel == kAudioDevicePropertySafetyOffset || sel == kAudioDevicePropertyNominalSampleRate ||
           sel == kAudioDevicePropertyAvailableNominalSampleRates || sel == kAudioDevicePropertyIsHidden ||
           sel == kAudioDevicePropertyZeroTimeStampPeriod || sel == kAudioDevicePropertyIcon ||
           sel == kAudioDevicePropertyPreferredChannelsForStereo || sel == kAudioDevicePropertyPreferredChannelLayout;
  }
  if (obj == kStreamIn) {
    return sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass ||
           sel == kAudioObjectPropertyOwner || sel == kAudioObjectPropertyOwnedObjects ||
           sel == kAudioStreamPropertyIsActive || sel == kAudioStreamPropertyDirection ||
           sel == kAudioStreamPropertyTerminalType || sel == kAudioStreamPropertyStartingChannel ||
           sel == kAudioStreamPropertyLatency || sel == kAudioStreamPropertyVirtualFormat ||
           sel == kAudioStreamPropertyPhysicalFormat || sel == kAudioStreamPropertyAvailableVirtualFormats ||
           sel == kAudioStreamPropertyAvailablePhysicalFormats;
  }
  return false;
}

OSStatus is_settable(AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* addr,
                     Boolean* out) {
  if (!addr || !out) return kAudioHardwareIllegalOperationError;
  *out = (obj == kBox && addr->mSelector == kAudioBoxPropertyAcquired) ||
         (obj == kStreamIn && (addr->mSelector == kAudioStreamPropertyIsActive ||
                               addr->mSelector == kAudioStreamPropertyVirtualFormat ||
                               addr->mSelector == kAudioStreamPropertyPhysicalFormat));
  return kAudioHardwareNoError;
}

static void fill_asbd(AudioStreamBasicDescription* asbd) {
  std::memset(asbd, 0, sizeof(*asbd));
  asbd->mSampleRate = kRate;
  asbd->mFormatID = kAudioFormatLinearPCM;
  asbd->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  asbd->mBitsPerChannel = 32;
  asbd->mChannelsPerFrame = kCh;
  asbd->mFramesPerPacket = 1;
  asbd->mBytesPerFrame = 4 * kCh;
  asbd->mBytesPerPacket = 4 * kCh;
}

OSStatus prop_size(AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* addr,
                   UInt32, const void*, UInt32* size) {
  if (!addr || !size) return kAudioHardwareIllegalOperationError;
  const auto sel = addr->mSelector;
  *size = 0;
  if (sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass || sel == kAudioObjectPropertyOwner ||
      sel == kAudioBoxPropertyTransportType || sel == kAudioBoxPropertyHasAudio || sel == kAudioBoxPropertyHasVideo ||
      sel == kAudioBoxPropertyHasMIDI || sel == kAudioBoxPropertyIsProtected || sel == kAudioBoxPropertyAcquired ||
      sel == kAudioBoxPropertyAcquisitionFailed || sel == kAudioDevicePropertyTransportType ||
      sel == kAudioDevicePropertyClockDomain || sel == kAudioDevicePropertyDeviceIsAlive ||
      sel == kAudioDevicePropertyDeviceIsRunning || sel == kAudioDevicePropertyDeviceCanBeDefaultDevice ||
      sel == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice || sel == kAudioDevicePropertyLatency ||
      sel == kAudioDevicePropertySafetyOffset || sel == kAudioDevicePropertyIsHidden ||
      sel == kAudioDevicePropertyZeroTimeStampPeriod || sel == kAudioStreamPropertyIsActive ||
      sel == kAudioStreamPropertyDirection || sel == kAudioStreamPropertyTerminalType ||
      sel == kAudioStreamPropertyStartingChannel || sel == kAudioStreamPropertyLatency) {
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyNominalSampleRate) {
    *size = sizeof(Float64);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioObjectPropertyManufacturer || sel == kAudioObjectPropertyName ||
      sel == kAudioPlugInPropertyResourceBundle || sel == kAudioBoxPropertyBoxUID ||
      sel == kAudioDevicePropertyDeviceUID || sel == kAudioDevicePropertyModelUID ||
      sel == kAudioPlugInPropertyTranslateUIDToBox || sel == kAudioPlugInPropertyTranslateUIDToDevice) {
    *size = sizeof(CFStringRef);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioPlugInPropertyBoxList ||
      sel == kAudioPlugInPropertyDeviceList || sel == kAudioBoxPropertyDeviceList ||
      sel == kAudioDevicePropertyRelatedDevices || sel == kAudioDevicePropertyStreams ||
      sel == kAudioObjectPropertyControlList) {
    if (obj == kPlugIn && (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioPlugInPropertyBoxList))
      *size = sizeof(AudioObjectID);
    else if (obj == kPlugIn && sel == kAudioPlugInPropertyDeviceList)
      *size = sizeof(AudioObjectID);
    else if (obj == kBox && (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioBoxPropertyDeviceList))
      *size = sizeof(AudioObjectID);
    else if (obj == kDevice && (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioDevicePropertyStreams ||
                                sel == kAudioDevicePropertyRelatedDevices))
      *size = sizeof(AudioObjectID);
    else
      *size = 0;
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyAvailableNominalSampleRates) {
    *size = sizeof(AudioValueRange);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyPreferredChannelsForStereo) {
    *size = sizeof(UInt32) * 2;
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyPreferredChannelLayout) {
    *size = offsetof(AudioChannelLayout, mChannelDescriptions) + sizeof(AudioChannelDescription);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyVirtualFormat || sel == kAudioStreamPropertyPhysicalFormat) {
    *size = sizeof(AudioStreamBasicDescription);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyAvailableVirtualFormats || sel == kAudioStreamPropertyAvailablePhysicalFormats) {
    *size = sizeof(AudioStreamRangedDescription);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyIcon) {
    *size = sizeof(CFURLRef);
    return kAudioHardwareNoError;
  }
  return kAudioHardwareUnknownPropertyError;
}

OSStatus get_prop(AudioServerPlugInDriverRef, AudioObjectID obj, pid_t, const AudioObjectPropertyAddress* addr, UInt32,
                  const void* qa, UInt32 /*inDataSize*/, UInt32* size, void* out) {
  if (!addr || !size || !out) return kAudioHardwareIllegalOperationError;
  const auto sel = addr->mSelector;
  (void)qa;
  if (sel == kAudioObjectPropertyBaseClass) {
    *reinterpret_cast<AudioClassID*>(out) =
        obj == kPlugIn ? kAudioObjectClassID : (obj == kBox ? kAudioBoxClassID : (obj == kDevice ? kAudioDeviceClassID : kAudioStreamClassID));
    *size = sizeof(AudioClassID);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioObjectPropertyClass) {
    *reinterpret_cast<AudioClassID*>(out) =
        obj == kPlugIn ? kAudioPlugInClassID
                       : (obj == kBox ? kAudioBoxClassID : (obj == kDevice ? kAudioDeviceClassID : kAudioStreamClassID));
    *size = sizeof(AudioClassID);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioObjectPropertyOwner) {
    *reinterpret_cast<AudioObjectID*>(out) = obj == kPlugIn ? kAudioObjectUnknown : (obj == kBox ? kPlugIn : (obj == kDevice ? kBox : kDevice));
    *size = sizeof(AudioObjectID);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioObjectPropertyManufacturer || sel == kAudioObjectPropertyName) {
    *reinterpret_cast<CFStringRef*>(out) = static_cast<CFStringRef>(CFRetain(sel == kAudioObjectPropertyName ? kName : CFSTR("尘埃X")));
    *size = sizeof(CFStringRef);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioPlugInPropertyBoxList ||
      sel == kAudioPlugInPropertyDeviceList || sel == kAudioBoxPropertyDeviceList || sel == kAudioDevicePropertyStreams ||
      sel == kAudioDevicePropertyRelatedDevices) {
    AudioObjectID id = kDevice;
    if (obj == kPlugIn && (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioPlugInPropertyBoxList)) id = kBox;
    if (obj == kDevice && sel == kAudioDevicePropertyStreams) id = kStreamIn;
    *reinterpret_cast<AudioObjectID*>(out) = id;
    *size = sizeof(AudioObjectID);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioPlugInPropertyTranslateUIDToBox) {
    *reinterpret_cast<AudioObjectID*>(out) = kBox;
    *size = sizeof(AudioObjectID);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioPlugInPropertyTranslateUIDToDevice) {
    *reinterpret_cast<AudioObjectID*>(out) = kDevice;
    *size = sizeof(AudioObjectID);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioPlugInPropertyResourceBundle) {
    *reinterpret_cast<CFStringRef*>(out) = static_cast<CFStringRef>(CFRetain(CFSTR("")));
    *size = sizeof(CFStringRef);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioBoxPropertyBoxUID || sel == kAudioDevicePropertyDeviceUID || sel == kAudioDevicePropertyModelUID) {
    CFStringRef s = sel == kAudioBoxPropertyBoxUID ? kBoxUid : kUid;
    *reinterpret_cast<CFStringRef*>(out) = static_cast<CFStringRef>(CFRetain(s));
    *size = sizeof(CFStringRef);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioBoxPropertyTransportType || sel == kAudioDevicePropertyTransportType) {
    *reinterpret_cast<UInt32*>(out) = kAudioDeviceTransportTypeVirtual;
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioBoxPropertyHasAudio || sel == kAudioBoxPropertyAcquired || sel == kAudioDevicePropertyDeviceIsAlive ||
      sel == kAudioDevicePropertyDeviceCanBeDefaultDevice || sel == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice) {
    *reinterpret_cast<UInt32*>(out) = 1;
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioBoxPropertyHasVideo || sel == kAudioBoxPropertyHasMIDI || sel == kAudioBoxPropertyIsProtected ||
      sel == kAudioBoxPropertyAcquisitionFailed || sel == kAudioDevicePropertyIsHidden ||
      sel == kAudioDevicePropertyLatency || sel == kAudioDevicePropertySafetyOffset || sel == kAudioDevicePropertyClockDomain ||
      sel == kAudioStreamPropertyLatency || sel == kAudioObjectPropertyControlList) {
    *reinterpret_cast<UInt32*>(out) = 0;
    *size = sel == kAudioObjectPropertyControlList ? 0 : sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyDeviceIsRunning) {
    *reinterpret_cast<UInt32*>(out) = 0;
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyZeroTimeStampPeriod) {
    *reinterpret_cast<UInt32*>(out) = 16384;
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyNominalSampleRate) {
    *reinterpret_cast<Float64*>(out) = kRate;
    *size = sizeof(Float64);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyAvailableNominalSampleRates) {
    auto* r = reinterpret_cast<AudioValueRange*>(out);
    r->mMinimum = kRate;
    r->mMaximum = kRate;
    *size = sizeof(AudioValueRange);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyPreferredChannelsForStereo) {
    auto* p = reinterpret_cast<UInt32*>(out);
    p[0] = 1;
    p[1] = 1;
    *size = sizeof(UInt32) * 2;
    return kAudioHardwareNoError;
  }
  if (sel == kAudioDevicePropertyPreferredChannelLayout) {
    auto* lay = reinterpret_cast<AudioChannelLayout*>(out);
    std::memset(lay, 0, *size);
    lay->mChannelLayoutTag = kAudioChannelLayoutTag_Mono;
    lay->mNumberChannelDescriptions = 1;
    *size = offsetof(AudioChannelLayout, mChannelDescriptions) + sizeof(AudioChannelDescription);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyIsActive) {
    *reinterpret_cast<UInt32*>(out) = 1;
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyDirection) {
    *reinterpret_cast<UInt32*>(out) = 1;  // input
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyTerminalType) {
    *reinterpret_cast<UInt32*>(out) = kAudioStreamTerminalTypeMicrophone;
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyStartingChannel) {
    *reinterpret_cast<UInt32*>(out) = 1;
    *size = sizeof(UInt32);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyVirtualFormat || sel == kAudioStreamPropertyPhysicalFormat) {
    fill_asbd(reinterpret_cast<AudioStreamBasicDescription*>(out));
    *size = sizeof(AudioStreamBasicDescription);
    return kAudioHardwareNoError;
  }
  if (sel == kAudioStreamPropertyAvailableVirtualFormats || sel == kAudioStreamPropertyAvailablePhysicalFormats) {
    auto* r = reinterpret_cast<AudioStreamRangedDescription*>(out);
    fill_asbd(&r->mFormat);
    r->mSampleRateRange.mMinimum = kRate;
    r->mSampleRateRange.mMaximum = kRate;
    *size = sizeof(AudioStreamRangedDescription);
    return kAudioHardwareNoError;
  }
  return kAudioHardwareUnknownPropertyError;
}

OSStatus set_prop(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32,
                  const void*, UInt32, const void*) {
  return kAudioHardwareNoError;
}

OSStatus start_io(AudioServerPlugInDriverRef ref, AudioObjectID, UInt32) {
  auto* d = as_driver(ref);
  if (!d) return kAudioHardwareBadObjectError;
  std::lock_guard<std::mutex> lock(d->mu);
  if (d->io_count == 0) {
    d->anchor_host = 0;
    d->anchor_sample = 0;
    if (!d->shm.valid()) d->shm.open_reader();
  }
  d->io_count++;
  return kAudioHardwareNoError;
}

OSStatus stop_io(AudioServerPlugInDriverRef ref, AudioObjectID, UInt32) {
  auto* d = as_driver(ref);
  if (!d) return kAudioHardwareBadObjectError;
  std::lock_guard<std::mutex> lock(d->mu);
  if (d->io_count > 0) d->io_count--;
  return kAudioHardwareNoError;
}

OSStatus get_zero_ts(AudioServerPlugInDriverRef ref, AudioObjectID, UInt32, Float64* hs, UInt64* hs_host, UInt64* seed) {
  auto* d = as_driver(ref);
  if (!d || !hs || !hs_host || !seed) return kAudioHardwareIllegalOperationError;
  const UInt64 period = 16384;
  mach_timebase_info_data_t tb{};
  mach_timebase_info(&tb);
  const Float64 host_ns = static_cast<Float64>(mach_absolute_time()) * tb.numer / tb.denom;
  if (d->anchor_host == 0) {
    d->anchor_host = host_ns;
    d->anchor_sample = 0;
  }
  const Float64 dt = (host_ns - d->anchor_host) / 1e9;
  const UInt64 samples = static_cast<UInt64>(dt * kRate);
  const UInt64 snapped = (samples / period) * period;
  *hs = static_cast<Float64>(snapped);
  *hs_host = static_cast<UInt64>(d->anchor_host + (static_cast<Float64>(snapped) / kRate) * 1e9);
  *seed = 1;
  return kAudioHardwareNoError;
}

OSStatus will_do_io(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 op, Boolean* will, Boolean* inplace) {
  if (!will || !inplace) return kAudioHardwareIllegalOperationError;
  const bool yes = op == kAudioServerPlugInIOOperationReadInput;
  *will = yes;
  *inplace = true;
  return kAudioHardwareNoError;
}

OSStatus begin_io(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
  return kAudioHardwareNoError;
}

OSStatus do_io(AudioServerPlugInDriverRef ref, AudioObjectID, AudioObjectID, UInt32, UInt32 op, UInt32 frames,
               const AudioServerPlugInIOCycleInfo*, void* main, void*) {
  auto* d = as_driver(ref);
  if (!d || !main) return kAudioHardwareIllegalOperationError;
  if (op != kAudioServerPlugInIOOperationReadInput) return kAudioHardwareNoError;
  auto* dst = static_cast<float*>(main);
  if (!d->shm.valid()) d->shm.open_reader();
  d->shm.read_pcm(dst, static_cast<int>(frames * kCh));
  return kAudioHardwareNoError;
}

OSStatus end_io(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) {
  return kAudioHardwareNoError;
}

}  // namespace

extern "C" void* AudioDriver_Create(CFAllocatorRef, CFUUIDRef type) {
  if (!type || !CFEqual(type, kAudioServerPlugInTypeUUID)) return nullptr;
  auto* d = new Driver();
  d->vtable = &d->iface;
  d->factory_uuid = CFUUIDCreateFromString(nullptr, CFSTR("A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F80"));
  d->iface.QueryInterface = drv_query;
  d->iface.AddRef = drv_add;
  d->iface.Release = drv_rel;
  d->iface.Initialize = initialize;
  d->iface.CreateDevice = create_device;
  d->iface.DestroyDevice = destroy_device;
  d->iface.AddDeviceClient = add_client;
  d->iface.RemoveDeviceClient = remove_client;
  d->iface.PerformDeviceConfigurationChange = +[](AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) -> OSStatus {
    return kAudioHardwareNoError;
  };
  d->iface.AbortDeviceConfigurationChange = +[](AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) -> OSStatus {
    return kAudioHardwareNoError;
  };
  d->iface.HasProperty = has_prop;
  d->iface.IsPropertySettable = is_settable;
  d->iface.GetPropertyDataSize = prop_size;
  d->iface.GetPropertyData = get_prop;
  d->iface.SetPropertyData = set_prop;
  d->iface.StartIO = start_io;
  d->iface.StopIO = stop_io;
  d->iface.GetZeroTimeStamp = get_zero_ts;
  d->iface.WillDoIOOperation = will_do_io;
  d->iface.BeginIOOperation = begin_io;
  d->iface.DoIOOperation = do_io;
  d->iface.EndIOOperation = end_io;
  d->iface._reserved = nullptr;
  return &d->vtable;
}
