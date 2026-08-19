#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <initguid.h>

#include "../../src/vcam_shm.hpp"

#include <atomic>
#include <cstring>
#include <string>

#ifndef KSPROPERTY_SUPPORT_GET
#define KSPROPERTY_SUPPORT_GET 1
#endif

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// {A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F70}
DEFINE_GUID(CLSID_DustXCam, 0xa71c3e80, 0x6d42, 0x4f1b, 0x9e, 0x3a, 0xc4, 0xd8, 0xb2, 0xe9, 0x1f, 0x70);

static const WCHAR kFilterName[] = L"尘埃X 摄像头";
static std::atomic<long> g_locks{0};

static const GUID kAmpin = {0x9b00f101, 0x1567, 0x11d1, {0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5}};

class DustXPin;
class DustXFilter;

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
  long refs_;
};

class DustXPin : public IPin, public IAMStreamConfig, public IKsPropertySet {
 public:
  explicit DustXPin(DustXFilter* filter);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp);
  ULONG STDMETHODCALLTYPE AddRef();
  ULONG STDMETHODCALLTYPE Release();

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

  HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE* mt) override;
  HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE** mt) override;
  HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int* count, int* size) override;
  HRESULT STDMETHODCALLTYPE GetStreamCaps(int i, AM_MEDIA_TYPE** mt, BYTE* caps) override;

  HRESULT STDMETHODCALLTYPE Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Get(REFGUID guid, DWORD pid, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD* written) override;
  HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID guid, DWORD pid, DWORD* type) override;

  HRESULT start();
  HRESULT stop();
  void fill_type(AM_MEDIA_TYPE* mt);

 private:
  static DWORD WINAPI thread_fn(LPVOID self);
  void run();
  long refs_ = 1;
  DustXFilter* filter_;
  IPin* peer_ = nullptr;
  IMemInputPin* input_ = nullptr;
  IMemAllocator* alloc_ = nullptr;
  HANDLE thread_ = nullptr;
  std::atomic<bool> running_{false};
  AM_MEDIA_TYPE type_{};
};

class DustXFilter : public ComBase<IBaseFilter>, public IAMFilterMiscFlags {
 public:
  DustXFilter();
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
  ULONG STDMETHODCALLTYPE GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* id) override {
    *id = CLSID_DustXCam;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stop() override;
  HRESULT STDMETHODCALLTYPE Pause() override;
  HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME) override;
  HRESULT STDMETHODCALLTYPE GetState(DWORD, FILTER_STATE* s) override {
    *s = state_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock* c) override {
    if (clock_) clock_->Release();
    clock_ = c;
    if (clock_) clock_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock** c) override {
    *c = clock_;
    if (*c) (*c)->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins** e) override;
  HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR, IPin** pin) override {
    pin_->AddRef();
    *pin = pin_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO* info) override;
  HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override {
    graph_ = graph;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }

  IFilterGraph* graph() const { return graph_; }

 private:
  ~DustXFilter() override;
  DustXPin* pin_ = nullptr;
  IFilterGraph* graph_ = nullptr;
  IReferenceClock* clock_ = nullptr;
  FILTER_STATE state_ = State_Stopped;
};

class EnumPins : public ComBase<IEnumPins> {
 public:
  explicit EnumPins(IPin* pin) : pin_(pin) { pin_->AddRef(); }
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
  IPin* pin_;
  ULONG pos_ = 0;
};

class EnumTypes : public ComBase<IEnumMediaTypes> {
 public:
  explicit EnumTypes(DustXPin* pin) : pin_(pin) { pin_->AddRef(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
      *pp = static_cast<IEnumMediaTypes*>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG n, AM_MEDIA_TYPE** mts, ULONG* fetched) override {
    ULONG got = 0;
    if (pos_ == 0 && n > 0) {
      auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
      pin_->fill_type(mt);
      mts[0] = mt;
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
  HRESULT STDMETHODCALLTYPE Clone(IEnumMediaTypes** e) override {
    *e = new EnumTypes(pin_);
    return S_OK;
  }

 private:
  ~EnumTypes() override { pin_->Release(); }
  DustXPin* pin_;
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
    auto* f = new DustXFilter();
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

static void free_mt(AM_MEDIA_TYPE* mt) {
  if (mt->cbFormat && mt->pbFormat) CoTaskMemFree(mt->pbFormat);
  if (mt->pUnk) mt->pUnk->Release();
  ZeroMemory(mt, sizeof(*mt));
}

void DustXPin::fill_type(AM_MEDIA_TYPE* mt) {
  ZeroMemory(mt, sizeof(*mt));
  mt->majortype = MEDIATYPE_Video;
  mt->subtype = MEDIASUBTYPE_RGB24;
  mt->formattype = FORMAT_VideoInfo;
  mt->bFixedSizeSamples = TRUE;
  mt->bTemporalCompression = FALSE;
  mt->lSampleSize = dustx::kVcamMaxBytes;
  mt->cbFormat = sizeof(VIDEOINFOHEADER);
  mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
  auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
  ZeroMemory(vih, sizeof(*vih));
  vih->AvgTimePerFrame = 10000000 / dustx::kVcamFps;
  vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  vih->bmiHeader.biWidth = dustx::kVcamWidth;
  vih->bmiHeader.biHeight = dustx::kVcamHeight;
  vih->bmiHeader.biPlanes = 1;
  vih->bmiHeader.biBitCount = 24;
  vih->bmiHeader.biCompression = BI_RGB;
  vih->bmiHeader.biSizeImage = dustx::kVcamMaxBytes;
  SetRect(&vih->rcSource, 0, 0, dustx::kVcamWidth, dustx::kVcamHeight);
  SetRect(&vih->rcTarget, 0, 0, dustx::kVcamWidth, dustx::kVcamHeight);
}

DustXPin::DustXPin(DustXFilter* filter) : filter_(filter) { fill_type(&type_); }

ULONG DustXPin::AddRef() { return InterlockedIncrement(&refs_); }
ULONG DustXPin::Release() {
  long n = InterlockedDecrement(&refs_);
  if (n == 0) delete this;
  return n;
}

HRESULT DustXPin::QueryInterface(REFIID riid, void** pp) {
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

HRESULT DustXPin::Connect(IPin* receive, const AM_MEDIA_TYPE* mt) {
  if (peer_) return VFW_E_ALREADY_CONNECTED;
  AM_MEDIA_TYPE use{};
  fill_type(&use);
  if (mt && FAILED(QueryAccept(mt))) {
    free_mt(&use);
    return VFW_E_TYPE_NOT_ACCEPTED;
  }
  HRESULT hr = receive->ReceiveConnection(this, mt ? mt : &use);
  if (FAILED(hr)) {
    free_mt(&use);
    return hr;
  }
  receive->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input_));
  if (!input_) {
    receive->Disconnect();
    free_mt(&use);
    return E_FAIL;
  }
  input_->GetAllocator(&alloc_);
  if (!alloc_) CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                                reinterpret_cast<void**>(&alloc_));
  ALLOCATOR_PROPERTIES want{}, actual{};
  want.cBuffers = 3;
  want.cbBuffer = dustx::kVcamMaxBytes;
  want.cbAlign = 1;
  alloc_->SetProperties(&want, &actual);
  alloc_->Commit();
  input_->NotifyAllocator(alloc_, FALSE);
  peer_ = receive;
  peer_->AddRef();
  if (mt) {
    free_mt(&type_);
    fill_type(&type_);
  }
  free_mt(&use);
  return S_OK;
}

HRESULT DustXPin::Disconnect() {
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

HRESULT DustXPin::ConnectedTo(IPin** pin) {
  if (!peer_) return VFW_E_NOT_CONNECTED;
  peer_->AddRef();
  *pin = peer_;
  return S_OK;
}

HRESULT DustXPin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
  *mt = type_;
  if (type_.cbFormat) {
    mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(type_.cbFormat));
    memcpy(mt->pbFormat, type_.pbFormat, type_.cbFormat);
  }
  return S_OK;
}

HRESULT DustXPin::QueryPinInfo(PIN_INFO* info) {
  ZeroMemory(info, sizeof(*info));
  info->pFilter = filter_;
  filter_->AddRef();
  info->dir = PINDIR_OUTPUT;
  wcscpy_s(info->achName, L"Output");
  return S_OK;
}

HRESULT DustXPin::QueryId(LPWSTR* id) {
  *id = static_cast<WCHAR*>(CoTaskMemAlloc(sizeof(WCHAR) * 8));
  wcscpy_s(*id, 8, L"Output");
  return S_OK;
}

HRESULT DustXPin::QueryAccept(const AM_MEDIA_TYPE* mt) {
  if (mt->majortype != MEDIATYPE_Video) return S_FALSE;
  if (mt->subtype != MEDIASUBTYPE_RGB24 && mt->subtype != GUID_NULL) return S_FALSE;
  return S_OK;
}

HRESULT DustXPin::EnumMediaTypes(IEnumMediaTypes** e) {
  *e = new EnumTypes(this);
  return S_OK;
}

HRESULT DustXPin::SetFormat(AM_MEDIA_TYPE* mt) { return QueryAccept(mt); }

HRESULT DustXPin::GetFormat(AM_MEDIA_TYPE** mt) {
  *mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
  fill_type(*mt);
  return S_OK;
}

HRESULT DustXPin::GetNumberOfCapabilities(int* count, int* size) {
  *count = 1;
  *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
  return S_OK;
}

HRESULT DustXPin::GetStreamCaps(int i, AM_MEDIA_TYPE** mt, BYTE* caps) {
  if (i != 0) return S_FALSE;
  *mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
  fill_type(*mt);
  auto* c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(caps);
  ZeroMemory(c, sizeof(*c));
  c->guid = FORMAT_VideoInfo;
  c->InputSize.cx = dustx::kVcamWidth;
  c->InputSize.cy = dustx::kVcamHeight;
  c->MinOutputSize = c->MaxOutputSize = c->InputSize;
  c->MinFrameInterval = c->MaxFrameInterval = 10000000 / dustx::kVcamFps;
  return S_OK;
}

HRESULT DustXPin::Get(REFGUID guid, DWORD pid, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD* written) {
  if (guid == kAmpin && pid == 0 && cb >= sizeof(GUID)) {
    memcpy(data, &PIN_CATEGORY_CAPTURE, sizeof(GUID));
    if (written) *written = sizeof(GUID);
    return S_OK;
  }
  return E_NOTIMPL;
}

HRESULT DustXPin::QuerySupported(REFGUID guid, DWORD pid, DWORD* type) {
  if (guid == kAmpin && pid == 0) {
    *type = KSPROPERTY_SUPPORT_GET;
    return S_OK;
  }
  return E_NOTIMPL;
}

HRESULT DustXPin::start() {
  if (running_) return S_OK;
  running_ = true;
  thread_ = CreateThread(nullptr, 0, thread_fn, this, 0, nullptr);
  return S_OK;
}

HRESULT DustXPin::stop() {
  running_ = false;
  if (thread_) {
    WaitForSingleObject(thread_, 1000);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  return S_OK;
}

DWORD WINAPI DustXPin::thread_fn(LPVOID self) {
  static_cast<DustXPin*>(self)->run();
  return 0;
}

static void rgb_to_bgr24_bottom(const uint8_t* rgb, BYTE* dest) {
  const int w = dustx::kVcamWidth;
  const int h = dustx::kVcamHeight;
  for (int y = 0; y < h; ++y) {
    const uint8_t* src = rgb + static_cast<size_t>(h - 1 - y) * w * 3;
    BYTE* row = dest + static_cast<size_t>(y) * w * 3;
    for (int x = 0; x < w; ++x) {
      row[x * 3 + 0] = src[x * 3 + 2];
      row[x * 3 + 1] = src[x * 3 + 1];
      row[x * 3 + 2] = src[x * 3 + 0];
    }
  }
}

static void fill_bars(BYTE* dest, uint32_t tick) {
  static const uint8_t bars[7][3] = {
      {192, 192, 192}, {0, 192, 192}, {192, 192, 0}, {0, 192, 0},
      {192, 0, 192},   {0, 0, 192},   {192, 0, 0},
  };
  const int w = dustx::kVcamWidth;
  const int h = dustx::kVcamHeight;
  uint8_t rgb[dustx::kVcamMaxBytes];
  for (int y = 0; y < h; ++y) {
    uint8_t* row = rgb + static_cast<size_t>(y) * w * 3;
    for (int x = 0; x < w; ++x) {
      const int b = (x * 7) / w;
      row[x * 3 + 0] = bars[b][0];
      row[x * 3 + 1] = bars[b][1];
      row[x * 3 + 2] = bars[b][2];
    }
  }
  const int stripe = static_cast<int>(tick % static_cast<uint32_t>(w));
  for (int y = h / 2 - 6; y < h / 2 + 6; ++y) {
    uint8_t* p = rgb + static_cast<size_t>(y) * w * 3 + static_cast<size_t>(stripe) * 3;
    p[0] = p[1] = p[2] = 255;
  }
  rgb_to_bgr24_bottom(rgb, dest);
}

void DustXPin::run() {
  dustx::VcamShm shm;
  shm.open_reader();
  uint8_t rgb[dustx::kVcamMaxBytes];
  uint32_t tick = 0;
  REFERENCE_TIME frame = 10000000 / dustx::kVcamFps;
  REFERENCE_TIME pts = 0;
  while (running_) {
    uint32_t id = 0;
    const bool have = shm.read_rgb(rgb, nullptr, nullptr, &id);
    if (alloc_ && input_) {
      IMediaSample* sample = nullptr;
      if (SUCCEEDED(alloc_->GetBuffer(&sample, nullptr, nullptr, 0)) && sample) {
        BYTE* dest = nullptr;
        sample->GetPointer(&dest);
        if (dest) {
          if (have) rgb_to_bgr24_bottom(rgb, dest);
          else fill_bars(dest, tick);
        }
        sample->SetActualDataLength(dustx::kVcamMaxBytes);
        sample->SetSyncPoint(TRUE);
        REFERENCE_TIME end = pts + frame;
        sample->SetTime(&pts, &end);
        input_->Receive(sample);
        sample->Release();
        pts = end;
      }
    }
    tick++;
    Sleep(1000 / dustx::kVcamFps);
  }
}

DustXFilter::DustXFilter() { pin_ = new DustXPin(this); }

DustXFilter::~DustXFilter() {
  pin_->stop();
  pin_->Release();
  if (clock_) clock_->Release();
}

HRESULT DustXFilter::QueryInterface(REFIID riid, void** pp) {
  if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
    *pp = static_cast<IBaseFilter*>(this);
  } else if (riid == IID_IAMFilterMiscFlags) {
    *pp = static_cast<IAMFilterMiscFlags*>(this);
  } else {
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT DustXFilter::Stop() {
  pin_->stop();
  state_ = State_Stopped;
  return S_OK;
}
HRESULT DustXFilter::Pause() {
  state_ = State_Paused;
  return S_OK;
}
HRESULT DustXFilter::Run(REFERENCE_TIME) {
  pin_->start();
  state_ = State_Running;
  return S_OK;
}

HRESULT DustXFilter::EnumPins(IEnumPins** e) {
  *e = new EnumPins(pin_);
  return S_OK;
}

HRESULT DustXFilter::QueryFilterInfo(FILTER_INFO* info) {
  ZeroMemory(info, sizeof(*info));
  wcscpy_s(info->achName, kFilterName);
  info->pGraph = graph_;
  if (graph_) graph_->AddRef();
  return S_OK;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** pp) {
  if (clsid != CLSID_DustXCam) return CLASS_E_CLASSNOTAVAILABLE;
  auto* f = new Factory();
  HRESULT hr = f->QueryInterface(riid, pp);
  f->Release();
  return hr;
}

STDAPI DllCanUnloadNow() { return g_locks == 0 ? S_OK : S_FALSE; }

static bool write_sz(HKEY root, const wchar_t* path, const wchar_t* name, const wchar_t* value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  LONG ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                           static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return ok == ERROR_SUCCESS;
}

STDAPI DllRegisterServer() {
  wchar_t dll[MAX_PATH];
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&DllRegisterServer), &self);
  GetModuleFileNameW(self, dll, MAX_PATH);
  const wchar_t* clsid = L"{A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F70}";
  const wchar_t* cat = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";
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
  const wchar_t* clsid = L"{A71C3E80-6D42-4F1B-9E3A-C4D8B2E91F70}";
  const wchar_t* cat = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";
  std::wstring inst = std::wstring(L"Software\\Classes\\CLSID\\") + cat + L"\\Instance\\" + clsid;
  RegDeleteTreeW(HKEY_CURRENT_USER, inst.c_str());
  std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
  RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
  return S_OK;
}
