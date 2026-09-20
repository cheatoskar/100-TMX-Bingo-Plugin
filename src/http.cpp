#include "http.h"

#include "tmx_version.h"

#include <windows.h>
#include <winhttp.h>

#include <cwchar>
#include <iterator>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace tmx {
namespace {

std::wstring widen(const std::string& in) {
  if (in.empty()) return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), &out[0], size);
  return out;
}

std::string narrow(const std::wstring& in) {
  if (in.empty()) return std::string();
  int size = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), &out[0], size, nullptr, nullptr);
  return out;
}

// One handle closed exactly once, however the function leaves. There is no
// exception path here, but there are seven early returns.
struct Handle {
  HINTERNET h = nullptr;
  explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  operator HINTERNET() const { return h; }
};

std::string header(HINTERNET request, DWORD which) {
  DWORD size = 0;
  WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return std::string();
  std::wstring buffer(size / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, &buffer[0], &size, WINHTTP_NO_HEADER_INDEX)) {
    return std::string();
  }
  buffer.resize(wcslen(buffer.c_str()));
  return narrow(buffer);
}

}  // namespace

Response request(const std::string& method,
                 const std::string& url,
                 const std::string& body,
                 const std::string& bearer,
                 bool followRedirects,
                 int timeoutMs) {
  Response out;

  std::wstring wideUrl = widen(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{};
  wchar_t path[2048]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));

  if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
    out.error = "bad url";
    return out;
  }

  Handle session(WinHttpOpen(L"100tmx-mod/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    out.error = "no winhttp session";
    return out;
  }
  WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

  Handle connection(WinHttpConnect(session, host, parts.nPort, 0));
  if (!connection) {
    out.error = "cannot reach the site";
    return out;
  }

  DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
  Handle request(WinHttpOpenRequest(connection, widen(method).c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
  if (!request) {
    out.error = "cannot open request";
    return out;
  }

  if (!followRedirects) {
    DWORD policy = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &policy, sizeof(policy));
  }

  std::wstring headers = L"Content-Type: application/json\r\n";
  if (!bearer.empty()) headers += L"Authorization: Bearer " + widen(bearer) + L"\r\n";
  // Which version is asking. The site needs it to refuse a capture from a
  // build with a known defect - 0.8.0 and earlier read a paused run as a
  // finished one, so a tile could be taken with a time nobody drove. A client
  // can of course lie about this; that is not the point. The point is that an
  // honest old build identifies itself and is turned away rather than quietly
  // putting a wrong time on somebody's board.
  headers += L"X-TMX-Mod: " + std::wstring(TMX_VERSION) + L"\r\n";

  if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                          body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                          static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) {
    out.error = "the site did not answer";
    return out;
  }

  if (!WinHttpReceiveResponse(request, nullptr)) {
    out.error = "no response";
    return out;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
  out.status = static_cast<int>(status);
  out.location = header(request, WINHTTP_QUERY_LOCATION);

  // Bounded: a response this mod cares about is a few kilobytes, and a bug at
  // the other end must not be able to grow the game's heap without limit.
  const size_t kMaxBody = 512 * 1024;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
    std::vector<char> chunk(available);
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
    out.body.append(chunk.data(), read);
    if (out.body.size() > kMaxBody) break;
  }

  out.ok = true;
  return out;
}

}  // namespace tmx
