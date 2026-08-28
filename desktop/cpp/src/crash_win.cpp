#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "crash_win.hpp"

#include "log.hpp"
#include "version.hpp"

#include <dbghelp.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace dustx {
namespace {

struct DumpJob {
  EXCEPTION_POINTERS* ep = nullptr;
  DWORD tid = 0;
  wchar_t path[MAX_PATH]{};
};

LONG g_dumping = 0;
wchar_t g_dir[MAX_PATH]{};

void fill_dir() {
  if (g_dir[0]) return;
  wchar_t appdata[MAX_PATH];
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) {
    wcscpy_s(g_dir, L".\\DustX");
    CreateDirectoryW(g_dir, nullptr);
    return;
  }
  _snwprintf_s(g_dir, _TRUNCATE, L"%s\\DustX", appdata);
  CreateDirectoryW(g_dir, nullptr);
}

void write_note(const wchar_t* dump, const char* why, DWORD code) {
  wchar_t note[MAX_PATH];
  _snwprintf_s(note, _TRUNCATE, L"%s\\crash.txt", g_dir);
  HANDLE f = CreateFileW(note, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  char dmp_utf8[MAX_PATH * 3];
  dmp_utf8[0] = 0;
  if (dump) WideCharToMultiByte(CP_UTF8, 0, dump, -1, dmp_utf8, sizeof(dmp_utf8), nullptr, nullptr);
  char buf[1536];
  const int n = std::snprintf(buf, sizeof(buf),
                              "version=%s\r\nwhy=%s\r\ncode=0x%08X\r\npid=%lu\r\ntid=%lu\r\ndmp=%s\r\n", kAppVersion,
                              why ? why : "?", static_cast<unsigned>(code), GetCurrentProcessId(), GetCurrentThreadId(),
                              dmp_utf8);
  DWORD wrote = 0;
  if (n > 0) WriteFile(f, buf, static_cast<DWORD>(n), &wrote, nullptr);
  CloseHandle(f);
}

DWORD WINAPI dump_thread(LPVOID raw) {
  auto* job = static_cast<DumpJob*>(raw);
  HANDLE file = CreateFileW(job->path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return 1;
  MINIDUMP_EXCEPTION_INFORMATION info{};
  MINIDUMP_EXCEPTION_INFORMATION* infop = nullptr;
  if (job->ep) {
    info.ThreadId = job->tid;
    info.ExceptionPointers = job->ep;
    info.ClientPointers = FALSE;
    infop = &info;
  }
  const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                               MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, infop, nullptr, nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);
  return 0;
}

bool dump_now(EXCEPTION_POINTERS* ep, const char* why) {
  if (InterlockedCompareExchange(&g_dumping, 1, 0) != 0) return false;
  fill_dir();
  SYSTEMTIME st{};
  GetLocalTime(&st);
  DumpJob job;
  job.ep = ep;
  job.tid = GetCurrentThreadId();
  _snwprintf_s(job.path, _TRUNCATE, L"%s\\crash-%04d%02d%02d-%02d%02d%02d-%lu.dmp", g_dir, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
  const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
  HANDLE th = CreateThread(nullptr, 0, dump_thread, &job, 0, nullptr);
  if (th) {
    WaitForSingleObject(th, 20000);
    CloseHandle(th);
  } else {
    dump_thread(&job);
  }
  write_note(job.path, why, code);
  char dmp_utf8[MAX_PATH * 3];
  dmp_utf8[0] = 0;
  WideCharToMultiByte(CP_UTF8, 0, job.path, -1, dmp_utf8, sizeof(dmp_utf8), nullptr, nullptr);
  char line[700];
  std::snprintf(line, sizeof(line), "写出崩溃转储 %s why=%s code=0x%08X", dmp_utf8, why ? why : "?",
                static_cast<unsigned>(code));
  log_error("crash", line);
  return true;
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
  dump_now(ep, "unhandled");
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK on_vectored(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN
      dump_now(ep, "vectored");
      break;
    default:
      break;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void on_terminate() {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  RtlCaptureContext(&ctx);
  EXCEPTION_RECORD rec{};
  rec.ExceptionCode = 0xE06D7363;
  EXCEPTION_POINTERS ep{};
  ep.ExceptionRecord = &rec;
  ep.ContextRecord = &ctx;
  dump_now(&ep, "terminate");
  std::abort();
}

void on_abort_signal(int) {
  dump_now(nullptr, "abort");
  std::_Exit(3);
}

}  // namespace

void install_crash_handler() {
  fill_dir();
  AddVectoredExceptionHandler(1, on_vectored);
  SetUnhandledExceptionFilter(on_unhandled);
  std::set_terminate(on_terminate);
  std::signal(SIGABRT, on_abort_signal);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

std::string crash_dir() {
  fill_dir();
  return wide_to_utf8(g_dir);
}

bool write_minidump(EXCEPTION_POINTERS* ep, const char* why) { return dump_now(ep, why); }

}  // namespace dustx
