// HTTPS, on the worker thread and nowhere else.
//
// WinHTTP because it is in Windows and needs nothing shipped alongside. Every
// call has a timeout: a request that hangs must never become a game that hangs,
// which is why nothing here is ever called from the render path (see the note
// in mod/README.md).
#pragma once

#include <string>

namespace tmx {

struct Response {
  bool ok = false;           // the request completed; says nothing about `status`
  int status = 0;            // HTTP status, 0 when the request never got that far
  std::string body;
  std::string location;      // the Location header, for the ManiaCode redirect
  std::string error;         // human-readable, shown in the status line
};

// `bearer` empty means no Authorization header. `followRedirects` is off for
// the play link: its whole value IS the redirect, which points at `tmtp://` -
// a scheme WinHTTP cannot follow and must not try to.
Response request(const std::string& method,
                 const std::string& url,
                 const std::string& body,
                 const std::string& bearer,
                 bool followRedirects = true,
                 int timeoutMs = 8000);

inline Response get(const std::string& url, const std::string& bearer = "") {
  return request("GET", url, "", bearer);
}

inline Response post(const std::string& url, const std::string& body, const std::string& bearer = "") {
  return request("POST", url, body, bearer);
}

}  // namespace tmx
