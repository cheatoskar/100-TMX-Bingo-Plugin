#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

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

/*
 * Writing happens on a thread of its own, and nothing else ever waits for it.
 *
 * It used to happen inline, under the lock, on whichever thread logged - and
 * on 2026-09-23 both the game reader and the worker sat for 17 seconds between
 * two log lines, right after a finish that set a new record, while the game
 * kept running. A line is a file append plus OutputDebugStringA, and the
 * second one is documented to wait up to 10 s per call for any debug-output
 * listener on the machine to take the previous string. So the logger could
 * stall the very thread that watches for finishes, through a whole results
 * screen. Now a line is stamped when it is said and queued; the writer below
 * pays whatever the disk and the listener cost, and says so when it was slow.
 */
struct Pending {
  SYSTEMTIME at{};
  std::string text;
};
std::mutex g_queueLock;
std::condition_variable g_queueReady;
std::deque<Pending> g_queue;
bool g_writerStarted = false;

void writeNow(const Pending& line) {
  const SYSTEMTIME& at = line.at;
  if (FILE* f = nullptr; fopen_s(&f, g_path.c_str(), "a") == 0 && f) {
    fprintf(f, "%02d:%02d:%02d.%03d  %s\n", at.wHour, at.wMinute, at.wSecond, at.wMilliseconds, line.text.c_str());
    fclose(f);  // closed per line: a crash must not lose the line that explains it
  }
}

void writer() {
  for (;;) {
    Pending line;
    {
      std::unique_lock<std::mutex> guard(g_queueLock);
      g_queueReady.wait(guard, [] { return !g_queue.empty(); });
      line = std::move(g_queue.front());
      g_queue.pop_front();
    }
    const ULONGLONG t0 = GetTickCount64();
    writeNow(line);
    const ULONGLONG t1 = GetTickCount64();
    OutputDebugStringA(("[100TMX] " + line.text + "\n").c_str());
    const ULONGLONG t2 = GetTickCount64();
    if (t2 - t0 > 250) {
      Pending note;
      GetLocalTime(&note.at);
      char text[160];
      sprintf_s(text, sizeof(text), "log: that line took %llu ms to write (file %llu ms, debug output %llu ms)",
                t2 - t0, t1 - t0, t2 - t1);
      note.text = text;
      writeNow(note);
    }
  }
}

// Called with g_lock held; costs a queue push and nothing else.
void write(const std::string& text) {
  Pending line;
  GetLocalTime(&line.at);
  line.text = text;
  {
    std::lock_guard<std::mutex> guard(g_queueLock);
    if (g_queue.size() < 2000) g_queue.push_back(std::move(line));
  }
  g_queueReady.notify_one();
  if (!g_writerStarted) {
    g_writerStarted = true;
    // Never joined: it lives as long as the process and goes down with it.
    // Started from DllMain is fine - it simply begins once the loader lock is
    // released, and nothing waits for it.
    std::thread(writer).detach();
  }
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
