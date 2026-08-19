#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <initguid.h>

#include "../../src/vmic_shm.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef KSPROPERTY_SUPPORT_GET
#define KSPROPERTY_SUPPORT_GET 1
#endif

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

DEFINE_GUID(CLSID_DustXMic, 0xa71c3e80, 0x6d42, 0x4f1b, 0x9e, 0x3a, 0xc4, 0xd8, 0xb2, 0xe9, 0x1f, 0x81);

static const WCHAR kFilterName[] = L"尘埃X 麦克风";
static volatile LONG g_locks = 0;
static const GUID kAmpin = {0x9b00f101, 0x1567, 0x11d1, {0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5}};

constexpr int kRate = 48000;
constexpr int kChunk = 960;
constexpr int kBytes = kChunk * 2;

class Filter;
class Pin;

template <typename T>
class ComBase : public T {
 public:
  explicit ComBase(long extra = 0) : refs_(1 + extra) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    long n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }

 protected:
  virtual ~ComBase() = default;
  volatile LONG refs_;
};

class Pin : public IPin, public IAMStreamConfig, public IKsPropertySet {
 public:
  explicit Pin(Filter* f) : filter_(f) {}
  ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() {
    long n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp);
  HRESULT STDMETHODCALLTYPE Connect(IPin* receive, const AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE Disconnect() override;
  HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pin) override;
  HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* info) override;
  HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION* dir) override {
    *dir = PINDIR_OUTPUT;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* id) override;
  HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes** e) override;
  HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EndOfStream() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE BeginFlush() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE EndFlush() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE* mt) override { return QueryAccept(mt); }
  HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE** mt) override;
  HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int* count, int* size) override {
    *count = 1;
    *size = sizeof(AUDIO_STREAM_CONFIG_CAPS);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamCaps(int i, AM_MEDIA_TYPE** mt, BYTE* caps) override;
  HRESULT STDMETHODCALLTYPE Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Get(REFGUID guid, DWORD pid, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD* written) override;
  HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID guid, DWORD pid, DWORD* type) override;
  void start();
  void stop();
  void fill_type(AM_MEDIA_TYPE* mt);

 private:
  static DWORD WINAPI thread_fn(LPVOID self);
  void run();
  long refs_ = 1;
  Filter* filter_;
  IPin* peer_ = nullptr;
  IMemInputPin* input_ = nullptr;
  IMemAllocator* alloc_ = nullptr;
  HANDLE thread_ = nullptr;
  std::atomic<bool> running_{false};
};

class Filter : public ComBase<IBaseFilter>, public IAMFilterMiscFlags {
 public:
  Filter();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return ComBase<IBaseFilter>::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return ComBase<IBaseFilter>::Release(); }
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* id) override {
    *id = CLSID_DustXMic;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stop() override;
  HRESULT STDMETHODCALLTYPE Pause() override {
    state_ = State_Paused;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME) override;
  HRESULT STDMETHODCALLTYPE GetState(DWORD, FILTER_STATE* s) override {
    *s = state_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** c) override {
    *c = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** e) override;
  HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR, IPin** pin) override {
    pin_->AddRef();
    *pin = pin_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* info) override;
  HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* g, LPCWSTR) override {
    graph_ = g;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }
  ULONG STDMETHODCALLTYPE GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }
  IFilterGraph* graph_ = nullptr;

 private:
  ~Filter() override;
  FILTER_STATE state_ = State_Stopped;
  Pin* pin_ = nullptr;
};

class EnumPins : public ComBase<IEnumPins> {
 public:
  explicit EnumPins(Pin* p) : pin_(p) { pin_->AddRef(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (riid == IID_IUnknown || riid == IID_IEnumPins) {
      *pp = static_cast<IEnumPins*>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG n, IPin** pins, ULONG* fetched) override {
    ULONG got = 0;
    if (pos_ == 0 && n > 0) {
      pin_->AddRef();
      pins[0] = pin_;
      pos_ = 1;
      got = 1;
    }
    if (fetched) *fetched = got;
    return got == n ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG n) override {
    pos_ += n;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    pos_ = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IEnumPins** e) override {
    *e = new EnumPins(pin_);
    return S_OK;
  }

 private:
  ~EnumPins() override { pin_->Release(); }
  Pin* pin_;
  ULONG pos_ = 0;
};

class EnumTypes : public ComBase<IEnumMediaTypes> {
 public:
  explicit EnumTypes(Pin* p) : pin_(p) { pin_->AddRef(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
      *pp = static_cast<IEnumMediaTypes*>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG n, AM_MEDIA_TYPE** types, ULONG* fetched) override {
    ULONG got = 0;
    if (pos_ == 0 && n > 0) {
      types[0] = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
      pin_->fill_type(types[0]);
      pos_ = 1;
      got = 1;
    }
    if (fetched) *fetched = got;
    return got == n ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG) override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE Reset() override {
    pos_ = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IEnumMediaTypes** e) override {
    *e = new EnumTypes(pin_);
    return S_OK;
  }

 private:
  ~EnumTypes() override { pin_->Release(); }
  Pin* pin_;
  ULONG pos_ = 0;
};

class Factory : public ComBase<IClassFactory> {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *pp = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** pp) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* f = new Filter();
    HRESULT hr = f->QueryInterface(riid, pp);
    f->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    if (lock) InterlockedIncrement(&g_locks);
    else InterlockedDecrement(&g_locks);
    return S_OK;
  }
};

void Pin::fill_type(AM_MEDIA_TYPE* mt) {
  ZeroMemory(mt, sizeof(*mt));
  mt->majortype = MEDIATYPE_Audio;
  mt->subtype = MEDIASUBTYPE_PCM;
  mt->formattype = FORMAT_WaveFormatEx;
  mt->bFixedSizeSamples = TRUE;
  mt->lSampleSize = 2;
  mt->cbFormat = sizeof(WAVEFORMATEX);
  mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
  auto* w = reinterpret_cast<WAVEFORMATEX*>(mt->pbFormat);
  ZeroMemory(w, sizeof(*w));
  w->wFormatTag = WAVE_FORMAT_PCM;
  w->nChannels = 1;
  w->nSamplesPerSec = kRate;
  w->wBitsPerSample = 16;
  w->nBlockAlign = 2;
  w->nAvgBytesPerSec = kRate * 2;
}

HRESULT Pin::QueryInterface(REFIID riid, void** pp) {
  if (riid == IID_IUnknown || riid == IID_IPin) *pp = static_cast<IPin*>(this);
  else if (riid == IID_IAMStreamConfig) *pp = static_cast<IAMStreamConfig*>(this);
  else if (riid == IID_IKsPropertySet) *pp = static_cast<IKsPropertySet*>(this);
  else {
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT Pin::Connect(IPin* receive, const AM_MEDIA_TYPE* mt) {
  if (peer_) return VFW_E_ALREADY_CONNECTED;
  AM_MEDIA_TYPE local{};
  fill_type(&local);
  if (mt && QueryAccept(mt) != S_OK) {
    CoTaskMemFree(local.pbFormat);
    return VFW_E_TYPE_NOT_ACCEPTED;
  }
  HRESULT hr = receive->ReceiveConnection(this, mt ? mt : &local);
  if (FAILED(hr)) {
    CoTaskMemFree(local.pbFormat);
    return hr;
  }
  receive->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input_));
  if (!input_) {
    receive->Disconnect();
    CoTaskMemFree(local.pbFormat);
    return E_FAIL;
  }
  input_->GetAllocator(&alloc_);
  if (!alloc_) {
    CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                     reinterpret_cast<void**>(&alloc_));
  }
  if (alloc_) {
    ALLOCATOR_PROPERTIES want{}, actual{};
    want.cBuffers = 4;
    want.cbBuffer = kBytes;
    want.cbAlign = 2;
    alloc_->SetProperties(&want, &actual);
    alloc_->Commit();
    input_->NotifyAllocator(alloc_, FALSE);
  }
  peer_ = receive;
  peer_->AddRef();
  CoTaskMemFree(local.pbFormat);
  return S_OK;
}

HRESULT Pin::Disconnect() {
  stop();
  if (alloc_) {
    alloc_->Decommit();
    alloc_->Release();
    alloc_ = nullptr;
  }
  if (input_) {
    input_->Release();
    input_ = nullptr;
  }
  if (peer_) {
    peer_->Release();
    peer_ = nullptr;
  }
  return S_OK;
}

HRESULT Pin::ConnectedTo(IPin** pin) {
  if (!peer_) return VFW_E_NOT_CONNECTED;
  peer_->AddRef();
  *pin = peer_;
  return S_OK;
}

HRESULT Pin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
  fill_type(mt);
  return S_OK;
}

HRESULT Pin::QueryPinInfo(PIN_INFO* info) {
  ZeroMemory(info, sizeof(*info));
  info->pFilter = reinterpret_cast<IBaseFilter*>(filter_);
  info->pFilter->AddRef();
  info->dir = PINDIR_OUTPUT;
  wcscpy_s(info->achName, L"Capture");
  return S_OK;
}

HRESULT Pin::QueryId(LPWSTR* id) {
  *id = static_cast<WCHAR*>(CoTaskMemAlloc(16 * sizeof(WCHAR)));
  wcscpy_s(*id, 8, L"Capture");
  return S_OK;
}

HRESULT Pin::QueryAccept(const AM_MEDIA_TYPE* mt) {
  if (!mt || mt->majortype != MEDIATYPE_Audio) return S_FALSE;
  if (mt->subtype != MEDIASUBTYPE_PCM && mt->subtype != GUID_NULL) return S_FALSE;
  return S_OK;
}

HRESULT Pin::EnumMediaTypes(IEnumMediaTypes** e) {
  *e = new EnumTypes(this);
  return S_OK;
}

HRESULT Pin::GetFormat(AM_MEDIA_TYPE** mt) {
  *mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
  fill_type(*mt);
  return S_OK;
}

HRESULT Pin::GetStreamCaps(int i, AM_MEDIA_TYPE** mt, BYTE* caps) {
  if (i != 0) return S_FALSE;
  *mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
  fill_type(*mt);
  auto* c = reinterpret_cast<AUDIO_STREAM_CONFIG_CAPS*>(caps);
  ZeroMemory(c, sizeof(*c));
  c->guid = MEDIATYPE_Audio;
  c->MinimumChannels = c->MaximumChannels = 1;
  c->MinimumBitsPerSample = c->MaximumBitsPerSample = 16;
  c->MinimumSampleFrequency = c->MaximumSampleFrequency = kRate;
  return S_OK;
}

HRESULT Pin::Get(REFGUID guid, DWORD pid, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD* written) {
  if (guid == kAmpin && pid == 0 && cb >= sizeof(GUID)) {
    memcpy(data, &PIN_CATEGORY_CAPTURE, sizeof(GUID));
    if (written) *written = sizeof(GUID);
    return S_OK;
  }
  return E_NOTIMPL;
}

HRESULT Pin::QuerySupported(REFGUID guid, DWORD pid, DWORD* type) {
  if (guid == kAmpin && pid == 0) {
    *type = KSPROPERTY_SUPPORT_GET;
    return S_OK;
  }
  return E_NOTIMPL;
}

void Pin::start() {
  if (running_) return;
  running_ = true;
  thread_ = CreateThread(nullptr, 0, thread_fn, this, 0, nullptr);
}

void Pin::stop() {
  running_ = false;
  if (thread_) {
    WaitForSingleObject(thread_, 1000);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
}

DWORD WINAPI Pin::thread_fn(LPVOID self) {
  static_cast<Pin*>(self)->run();
  return 0;
}

void Pin::run() {
  dustx::VmicShm shm;
  shm.open_reader();
  float pcm[kChunk];
  REFERENCE_TIME pts = 0;
  const REFERENCE_TIME dur = 10000000 * kChunk / kRate;
  while (running_) {
    const int got = shm.read_pcm(pcm, kChunk);
    if (got <= 0) std::memset(pcm, 0, sizeof(pcm));
    if (alloc_ && input_) {
      IMediaSample* sample = nullptr;
      if (SUCCEEDED(alloc_->GetBuffer(&sample, nullptr, nullptr, 0)) && sample) {
        BYTE* dest = nullptr;
        sample->GetPointer(&dest);
        if (dest) {
          auto* s16 = reinterpret_cast<int16_t*>(dest);
          for (int i = 0; i < kChunk; ++i) {
            float v = pcm[i];
            if (v > 1.f) v = 1.f;
            if (v < -1.f) v = -1.f;
            s16[i] = static_cast<int16_t>(v * 32767.f);
          }
        }
        sample->SetActualDataLength(kBytes);
        sample->SetSyncPoint(TRUE);
        REFERENCE_TIME end = pts + dur;
        sample->SetTime(&pts, &end);
        input_->Receive(sample);
        sample->Release();
        pts = end;
      }
    }
    Sleep(20);
  }
}

Filter::Filter() { pin_ = new Pin(this); }
Filter::~Filter() {
  pin_->stop();
  pin_->Release();
}

HRESULT Filter::QueryInterface(REFIID riid, void** pp) {
  if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
    *pp = static_cast<IBaseFilter*>(this);
  } else if (riid == IID_IAMFilterMiscFlags) {
    *pp = static_cast<IAMFilterMiscFlags*>(this);
  } else {
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  ComBase<IBaseFilter>::AddRef();
  return S_OK;
}

HRESULT Filter::Stop() {
  pin_->stop();
  state_ = State_Stopped;
  return S_OK;
}

HRESULT Filter::Run(REFERENCE_TIME) {
  pin_->start();
  state_ = State_Running;
  return S_OK;
}

HRESULT Filter::EnumPins(IEnumPins** e) {
  *e = new ::EnumPins(pin_);
  return S_OK;
}

HRESULT Filter::QueryFilterInfo(FILTER_INFO* info) {
  ZeroMemory(info, sizeof(*info));
  wcscpy_s(info->achName, kFilterName);
  info->pGraph = graph_;
  if (graph_) graph_->AddRef();
  return S_OK;
}

static bool write_sz(HKEY root, const wchar_t* path, const wchar_t* name, const wchar_t* value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  LONG ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                           static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return ok == ERROR_SUCCESS;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** pp) {
  if (clsid != CLSID_DustXMic) return CLASS_E_CLASSNOTAVAILABLE;
  auto* f = new Factory();
  HRESULT hr = f->QueryInterface(riid, pp);
  f->Release();
  return hr;
}

STDAPI DllCanUnloadNow() { return g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
  wchar_t dll[MAX_PATH];
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&DllRegisterServer), &self);
  GetModuleFileNameW(self, dll, MAX_PATH);
  const wchar_t* clsid = L"{A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F81}";
  const wchar_t* cat = L"{33D9A762-90C8-11d0-BD43-00A0C911CE86}";
  std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
  write_sz(HKEY_CURRENT_USER, base.c_str(), nullptr, kFilterName);
  write_sz(HKEY_CURRENT_USER, (base + L"\\InprocServer32").c_str(), nullptr, dll);
  write_sz(HKEY_CURRENT_USER, (base + L"\\InprocServer32").c_str(), L"ThreadingModel", L"Both");
  std::wstring inst = std::wstring(L"Software\\Classes\\CLSID\\") + cat + L"\\Instance\\" + clsid;
  write_sz(HKEY_CURRENT_USER, inst.c_str(), L"FriendlyName", kFilterName);
  write_sz(HKEY_CURRENT_USER, inst.c_str(), L"CLSID", clsid);
  return S_OK;
}

STDAPI DllUnregisterServer() {
  const wchar_t* clsid = L"{A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F81}";
  const wchar_t* cat = L"{33D9A762-90C8-11d0-BD43-00A0C911CE86}";
  std::wstring inst = std::wstring(L"Software\\Classes\\CLSID\\") + cat + L"\\Instance\\" + clsid;
  RegDeleteTreeW(HKEY_CURRENT_USER, inst.c_str());
  std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
  RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
  return S_OK;
}
