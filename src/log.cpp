#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace tmx {
namespace log {
namespace {

std::mutex g_lock;
std::string g_path;
std::map<std::string, std::string> g_last;
bool g_ready = false;

const long long kMaxBytes = 1024 * 1024;

// Deliberately not using Config::path(): the log has to work before anything
// else is initialised, including the config it would otherwise depend on.
std::string logPath() {
  wchar_t* raw = nullptr;
  std::string dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw) {
    int size = WideCharToMultiByte(CP_UTF8, 0, raw, -1, nullptr, 0, nullptr, nullptr);
    dir.resize(static_cast<size_t>(size > 0 ? size - 1 : 0));
    WideCharToMultiByte(CP_UTF8, 0, raw, -1, &dir[0], size, nullptr, nullptr);
  }
  if (raw) CoTaskMemFree(raw);
  if (dir.empty()) return "100TMX-log.txt";
  dir += "\\100TMX";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\log.txt";
}

void ensureReady() {
  if (g_ready) return;
  g_ready = true;
  g_path = logPath();

  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (GetFileAttributesExA(g_path.c_str(), GetFileExInfoStandard, &info)) {
    long long size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > kMaxBytes) DeleteFileA(g_path.c_str());
  }

  SYSTEMTIME now{};
  GetLocalTime(&now);
  if (FILE* f = nullptr; fopen_s(&f, g_path.c_str(), "a") == 0 && f) {
    fprintf(f, "\n=== 100TMX mod started %04d-%02d-%02d %02d:%02d:%02d (pid %lu) ===\n", now.wYear, now.wMonth,
            now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    fclose(f);
  }
}

void write(const std::string& text) {
  SYSTEMTIME now{};
  GetLocalTime(&now);

  if (FILE* f = nullptr; fopen_s(&f, g_path.c_str(), "a") == 0 && f) {
    fprintf(f, "%02d:%02d:%02d.%03d  %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text.c_str());
    fclose(f);  // closed per line: a crash must not lose the line that explains it
  }
  OutputDebugStringA(("[100TMX] " + text + "\n").c_str());
}

std::string format(const char* fmt, va_list args) {
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  return buffer;
}

}  // namespace

void line(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::string text = format(fmt, args);
  va_end(args);

  std::lock_guard<std::mutex> guard(g_lock);
  ensureReady();
  write(text);
}

void once(const char* key, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::string text = format(fmt, args);
  va_end(args);

  std::lock_guard<std::mutex> guard(g_lock);
  ensureReady();
  auto it = g_last.find(key);
  if (it != g_last.end() && it->second == text) return;
  g_last[key] = text;
  write(text);
}

}  // namespace log
}  // namespace tmx
