#define _CRT_RAND_S
#include "bridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "json.h"
#include "log.h"
#include "state.h"
#include "tmx_version.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

namespace tmx {
namespace bridge {
namespace {

/**
 * The port, and the four after it.
 *
 * A fixed port means the extension does not have to be told one, and the
 * fallbacks mean a second TrackMania - or anything else that happened to take
 * 27311 - does not leave the bridge silently dead. The extension probes the
 * same five in the same order, which is the whole protocol for finding us.
 */
constexpr int kBasePort = 27311;
constexpr int kPortTries = 5;

/** A replay never waits long for collection; a queue this size is already a bug. */
constexpr size_t kMaxQueue = 8;

/**
 * How recent an autosave has to be to be the run just driven.
 *
 * TrackMania writes it as the finish screen appears, and this is called from
 * the worker's next tick, so the real gap is under a second. Ninety is for a
 * machine under load, and short enough that yesterday's file can never be
 * mistaken for today's.
 */
constexpr int kAutosaveAgeSeconds = 90;

struct Pending {
  std::string id;
  std::string path;
  std::string fileName;
  std::string site;
  int trackId = 0;
  std::string mapName;
  int timeMs = 0;
  /** The map's UID was found inside the file, so this is certainly its replay. */
  bool verified = false;
};

std::mutex g_lock;
std::deque<Pending> g_queue;
std::atomic<bool> g_running{false};
std::atomic<bool> g_paired{false};
std::atomic<int> g_port{0};
std::atomic<int> g_handlers{0};
SOCKET g_listener = INVALID_SOCKET;
std::thread g_thread;
std::string g_lastResult;
unsigned long long g_counter = 0;

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- the key

std::string randomKey() {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 32; i++) {
    unsigned int v = 0;
    // rand_s is the CryptGenRandom-backed one. This is a pairing secret for a
    // loopback socket rather than a credential, but "good enough" and "from
    // the OS" cost the same here.
    if (rand_s(&v) != 0) v = static_cast<unsigned int>(GetTickCount()) + i;
    out.push_back(kHex[v & 0xF]);
  }
  return out;
}

// ------------------------------------------------------- finding a replay

std::string documentsPath() {
  wchar_t buffer[MAX_PATH] = {0};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, buffer))) return "";
  char narrow[MAX_PATH * 2] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
  return narrow;
}

/**
 * Where TrackMania puts the replay it saved for you.
 *
 * The default user directory, unless the ini names another - a player running
 * the game with its own `-userdir`, or with Documents redirected somewhere
 * this cannot guess, sets `replay_dir` and is done. Both Nations and United
 * keep autosaves under the same relative path.
 */
std::string autosaveDir() {
  if (!config().replayDir.empty()) return config().replayDir;
  const std::string docs = documentsPath();
  if (docs.empty()) return "";
  return docs + "\\TmForever\\Tracks\\Replays\\Autosaves";
}

/** Does this file carry that map's UID? The UID sits in the header as plain text. */
bool fileMentions(const std::string& path, const std::string& uid) {
  if (uid.empty()) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  // The header is the first few kilobytes; a megabyte is far past generous and
  // still nothing next to reading the whole body.
  std::vector<char> buffer(64 * 1024);
  in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  const size_t read = static_cast<size_t>(in.gcount());
  if (read == 0) return false;
  const std::string haystack(buffer.data(), read);
  return haystack.find(uid) != std::string::npos;
}

struct Found {
  std::string path;
  std::string name;
  bool verified = false;
};

/**
 * The newest autosave written in the last minute and a half.
 *
 * Walked rather than guessed from the map's name: TrackMania sanitises the
 * name into the filename in ways that are tedious to reproduce exactly, while
 * "the file that appeared when you crossed the line" needs no reproduction at
 * all. Where the UID can be confirmed inside the file, that candidate wins
 * over a merely newer one - which is what stops the wrong replay being
 * uploaded under somebody's name after an alt-tab.
 */
bool findAutosave(const std::string& uid, Found& out) {
  const std::string dir = autosaveDir();
  if (dir.empty()) return false;

  WIN32_FIND_DATAA find{};
  const std::string pattern = dir + "\\*.Replay.Gbx";
  HANDLE handle = FindFirstFileA(pattern.c_str(), &find);
  if (handle == INVALID_HANDLE_VALUE) {
    log::once("autosave", "bridge: no autosaves in %s", dir.c_str());
    return false;
  }

  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const unsigned long long nowTicks =
      (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  const unsigned long long window = static_cast<unsigned long long>(kAutosaveAgeSeconds) * 10000000ULL;

  unsigned long long best = 0;
  bool bestVerified = false;
  do {
    if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const unsigned long long written =
        (static_cast<unsigned long long>(find.ftLastWriteTime.dwHighDateTime) << 32) |
        find.ftLastWriteTime.dwLowDateTime;
    if (written + window < nowTicks) continue;

    const std::string path = dir + "\\" + find.cFileName;
    const bool verified = fileMentions(path, uid);
    // A confirmed file always beats an unconfirmed one, however new.
    if (bestVerified && !verified) continue;
    if (verified == bestVerified && written <= best) continue;

    best = written;
    bestVerified = verified;
    out.path = path;
    out.name = find.cFileName;
    out.verified = verified;
  } while (FindNextFileA(handle, &find));
  FindClose(handle);

  return best != 0;
}

// ------------------------------------------------------------ the answers

std::string httpResponse(int status, const std::string& contentType, const std::string& body) {
  const char* reason = status == 200 ? "OK" : status == 204 ? "No Content"
                       : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized"
                       : status == 404 ? "Not Found" : "Error";
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
  head += "Content-Type: " + contentType + "\r\n";
  head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  // No Access-Control-Allow-Origin, deliberately. The extension reaches us
  // through a host permission, which is not subject to CORS; a page in a
  // browser tab is, and there is no reason for one to read this.
  head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
  return head + body;
}

std::string jsonResponse(int status, const std::string& body) {
  return httpResponse(status, "application/json", body);
}

std::string queryValue(const std::string& query, const std::string& key) {
  size_t at = 0;
  while (at < query.size()) {
    const size_t end = query.find('&', at);
    const std::string pair = query.substr(at, end == std::string::npos ? std::string::npos : end - at);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return "";
}

std::string describe(const Pending& p) {
  return std::string("{\"id\":") + Json::quote(p.id) +
         ",\"site\":" + Json::quote(p.site) +
         ",\"trackId\":" + std::to_string(p.trackId) +
         ",\"mapName\":" + Json::quote(p.mapName) +
         ",\"fileName\":" + Json::quote(p.fileName) +
         ",\"timeMs\":" + std::to_string(p.timeMs) +
         ",\"verified\":" + (p.verified ? "true" : "false") + "}";
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void toast(const std::string& text, double seconds = 12.0) {
  shared().write([&](State& s) {
    s.toast = text;
    s.toastUntil = nowSeconds() + seconds;
  });
}

// -------------------------------------------------------------- one client

std::string handleRequest(const std::string& method,
                          const std::string& path,
                          const std::string& query,
                          const std::string& key,
                          const std::string& body) {
  if (key.empty() || key != config().bridgeKey) {
    return jsonResponse(401, "{\"ok\":false,\"error\":\"pair this browser with the key the mod shows\"}");
  }
  g_paired = true;

  if (method == "GET" && path == "/v1/hello") {
    const State view = shared().read();
    return jsonResponse(200, std::string("{\"ok\":true,\"mod\":") + Json::quote(TMX_VERSION_A) +
                                 ",\"game\":" + Json::quote(view.variant) +
                                 ",\"linked\":" + (view.linked ? "true" : "false") + "}");
  }

  if (method == "GET" && path == "/v1/next") {
    // A long poll rather than a fast one. The browser's worker stays alive
    // while a request is open, so this costs one parked connection instead of
    // a wake-up every second - and a finish reaches the extension in the time
    // it takes to answer, not on the next tick.
    int wait = atoi(queryValue(query, "wait").c_str());
    wait = std::max(0, std::min(wait, 55));
    const double deadline = nowSeconds() + wait;
    for (;;) {
      {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_queue.empty()) {
          return jsonResponse(200, "{\"ok\":true,\"upload\":" + describe(g_queue.front()) + "}");
        }
      }
      if (!g_running || nowSeconds() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return jsonResponse(200, "{\"ok\":true,\"upload\":null}");
  }

  if (method == "GET" && path == "/v1/file") {
    const std::string id = queryValue(query, "id");
    std::string file;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      for (const Pending& p : g_queue) {
        if (p.id == id) {
          file = p.path;
          break;
        }
      }
    }
    if (file.empty()) return jsonResponse(404, "{\"ok\":false,\"error\":\"no such replay\"}");
    const std::string bytes = readFile(file);
    if (bytes.empty()) return jsonResponse(404, "{\"ok\":false,\"error\":\"the file has gone\"}");
    return httpResponse(200, "application/octet-stream", bytes);
  }

  if (method == "POST" && path == "/v1/result") {
    const Json data = Json::parse(body);
    const std::string id = data.str("id");
    const bool ok = data.flag("ok");
    const std::string detail = data.str("detail");

    Pending done;
    bool found = false;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if (it->id != id) continue;
        done = *it;
        g_queue.erase(it);
        found = true;
        break;
      }
    }
    if (!found) return jsonResponse(404, "{\"ok\":false,\"error\":\"no such replay\"}");

    // Said in the overlay, because the player is in the game, not in the
    // browser. "Not faster than your own record" is TMX's ordinary answer on a
    // map they have already beaten and is not a failure of anything.
    const std::string what = ok ? "Replay uploaded to TMX." : (detail.empty() ? "TMX refused the replay." : detail);
    {
      std::lock_guard<std::mutex> guard(g_lock);
      g_lastResult = what;
    }
    toast(what);
    log::line("bridge: %s (%s)", what.c_str(), done.fileName.c_str());
    return jsonResponse(200, "{\"ok\":true}");
  }

  return jsonResponse(404, "{\"ok\":false,\"error\":\"unknown request\"}");
}

void serve(SOCKET client) {
  // Every read and write is bounded: a client that connects and says nothing
  // must not hold a thread for the life of the game.
  DWORD timeout = 60000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

  std::string request;
  char buffer[4096];
  size_t headerEnd = std::string::npos;
  while (request.size() < 32 * 1024) {
    const int got = recv(client, buffer, sizeof(buffer), 0);
    if (got <= 0) break;
    request.append(buffer, static_cast<size_t>(got));
    headerEnd = request.find("\r\n\r\n");
    if (headerEnd != std::string::npos) break;
  }

  if (headerEnd != std::string::npos) {
    const std::string head = request.substr(0, headerEnd);
    std::string body = request.substr(headerEnd + 4);

    // The request line.
    const size_t firstSpace = head.find(' ');
    const size_t secondSpace = head.find(' ', firstSpace + 1);
    std::string method, target;
    if (firstSpace != std::string::npos && secondSpace != std::string::npos) {
      method = head.substr(0, firstSpace);
      target = head.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    }

    // The two headers this speaks: the key, and how much body to wait for.
    std::string key;
    size_t contentLength = 0;
    size_t at = head.find("\r\n");
    while (at != std::string::npos) {
      const size_t end = head.find("\r\n", at + 2);
      const std::string line = head.substr(at + 2, end == std::string::npos ? std::string::npos : end - at - 2);
      const size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (name == "x-tmx-key") key = value;
        else if (name == "content-length") contentLength = static_cast<size_t>(atoi(value.c_str()));
      }
      at = end;
    }

    while (body.size() < contentLength && body.size() < 1024 * 1024) {
      const int got = recv(client, buffer, sizeof(buffer), 0);
      if (got <= 0) break;
      body.append(buffer, static_cast<size_t>(got));
    }

    const size_t mark = target.find('?');
    const std::string path = mark == std::string::npos ? target : target.substr(0, mark);
    const std::string query = mark == std::string::npos ? "" : target.substr(mark + 1);

    const std::string answer = handleRequest(method, path, query, key, body);
    size_t sent = 0;
    while (sent < answer.size()) {
      const int wrote = send(client, answer.data() + sent, static_cast<int>(answer.size() - sent), 0);
      if (wrote <= 0) break;
      sent += static_cast<size_t>(wrote);
    }
  }

  shutdown(client, SD_BOTH);
  closesocket(client);
  g_handlers--;
}

void listen_loop() {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    log::line("bridge: winsock would not start");
    g_running = false;
    return;
  }

  SOCKET listener = INVALID_SOCKET;
  int port = 0;
  for (int i = 0; i < kPortTries; i++) {
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) break;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    // Loopback only. Not INADDR_ANY: this hands out a file from the player's
    // disk, and the machine next to them on the wifi has no business asking.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(kBasePort + i));

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        listen(listener, 4) == 0) {
      port = kBasePort + i;
      break;
    }
    closesocket(listener);
    listener = INVALID_SOCKET;
  }

  if (listener == INVALID_SOCKET) {
    log::line("bridge: no free port in %d..%d", kBasePort, kBasePort + kPortTries - 1);
    g_running = false;
    WSACleanup();
    return;
  }

  g_listener = listener;
  g_port = port;
  log::line("bridge: listening on 127.0.0.1:%d", port);

  while (g_running) {
    const SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) break;  // closed on the way out, or a real error
    if (g_handlers.load() >= 4) {
      closesocket(client);
      continue;
    }
    g_handlers++;
    std::thread(serve, client).detach();
  }

  closesocket(listener);
  g_listener = INVALID_SOCKET;
  g_port = 0;
  WSACleanup();
  log::line("bridge: stopped");
}

}  // namespace

void start() {
  if (!config().bridge || g_running) return;
  if (config().bridgeKey.empty()) {
    config().bridgeKey = randomKey();
    config().save();
  }
  g_running = true;
  g_thread = std::thread(listen_loop);
}

void stop() {
  if (!g_running) return;
  g_running = false;
  // Closing the listening socket is what wakes `accept`; there is no portable
  // way to interrupt it otherwise, and a thread parked in accept would keep
  // the process alive after the game has gone.
  if (g_listener != INVALID_SOCKET) closesocket(g_listener);
  if (g_thread.joinable()) g_thread.join();
  std::lock_guard<std::mutex> guard(g_lock);
  g_queue.clear();
}

void setEnabled(bool on) {
  config().bridge = on;
  config().save();
  if (on) start();
  else stop();
}

Status status() {
  Status out;
  out.running = g_running;
  out.port = g_port;
  out.paired = g_paired;
  std::lock_guard<std::mutex> guard(g_lock);
  out.queued = static_cast<int>(g_queue.size());
  out.lastResult = g_lastResult;
  return out;
}

bool offerFinish(const std::string& site, int trackId, const std::string& mapName, const std::string& uid,
                 int timeMs) {
  if (!g_running || site.empty() || trackId <= 0) return false;

  Found found;
  if (!findAutosave(uid, found)) {
    log::once("autosave", "bridge: no autosave written in the last %ds - is autosaving on?", kAutosaveAgeSeconds);
    return false;
  }

  Pending pending;
  pending.id = std::to_string(++g_counter) + "-" + std::to_string(trackId);
  pending.path = found.path;
  pending.fileName = found.name;
  pending.site = site;
  pending.trackId = trackId;
  pending.mapName = mapName;
  pending.timeMs = timeMs;
  pending.verified = found.verified;

  {
    std::lock_guard<std::mutex> guard(g_lock);
    // The same file twice is the same finish reported twice; the queue is the
    // wrong place to notice that, but it is a cheap place to stop it.
    for (const Pending& p : g_queue) {
      if (p.path == pending.path) return false;
    }
    while (g_queue.size() >= kMaxQueue) g_queue.pop_front();
    g_queue.push_back(pending);
  }

  log::line("bridge: queued %s for upload (%s, %s)", found.name.c_str(), site.c_str(),
            found.verified ? "uid confirmed" : "uid not confirmed");
  toast(g_paired ? "Uploading your replay through the browser..."
                 : "Replay ready - open a TMX tab with the extension to upload it.");
  return true;
}

}  // namespace bridge
}  // namespace tmx
