// Just enough JSON to read what the site answers with.
//
// Header-only and deliberately small: the mod parses five response shapes and
// writes three request bodies, all of them flat. Pulling a library in for that
// would mean vendoring it into a game process where every dependency is also a
// thing that can crash somebody's evening.
//
// Numbers are kept as double and as the original text, because a track id is
// an integer that must survive the round trip exactly.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace tmx {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> array;
  std::map<std::string, Json> object;

  bool isNull() const { return type == Type::Null; }

  // Reading a field that is not there is normal - the site omits what does not
  // apply - so every accessor takes the answer for that case.
  const Json* find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }

  std::string str(const std::string& key, const std::string& fallback = "") const {
    const Json* v = find(key);
    return v && v->type == Type::String ? v->string : fallback;
  }

  double num(const std::string& key, double fallback = 0) const {
    const Json* v = find(key);
    return v && v->type == Type::Number ? v->number : fallback;
  }

  int integer(const std::string& key, int fallback = 0) const {
    return static_cast<int>(num(key, static_cast<double>(fallback)));
  }

  // Tri-state on purpose: "is this map open" has a third answer, "the catalogue
  // has never seen it", and the overlay must not print that as "finished".
  int tribool(const std::string& key) const {
    const Json* v = find(key);
    if (!v || v->type != Type::Bool) return -1;
    return v->boolean ? 1 : 0;
  }

  bool flag(const std::string& key, bool fallback = false) const {
    const Json* v = find(key);
    return v && v->type == Type::Bool ? v->boolean : fallback;
  }

  const Json* child(const std::string& key) const { return find(key); }

  static Json parse(const std::string& text) {
    size_t i = 0;
    Json out = parseValue(text, i);
    return out;
  }

  // ---------------------------------------------------------------- writing
  static std::string quote(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            sprintf_s(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += c;
          }
      }
    }
    out += "\"";
    return out;
  }

 private:
  static void skip(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
  }

  static Json parseValue(const std::string& s, size_t& i) {
    skip(s, i);
    if (i >= s.size()) return Json();

    switch (s[i]) {
      case '{': return parseObject(s, i);
      case '[': return parseArray(s, i);
      case '"': {
        Json v;
        v.type = Type::String;
        v.string = parseString(s, i);
        return v;
      }
      case 't':
        i += 4;
        { Json v; v.type = Type::Bool; v.boolean = true; return v; }
      case 'f':
        i += 5;
        { Json v; v.type = Type::Bool; v.boolean = false; return v; }
      case 'n':
        i += 4;
        return Json();
      default: return parseNumber(s, i);
    }
  }

  static std::string parseString(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    i++;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        i++;
        switch (s[i]) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            // Enough of UTF-16 to not mangle a map name: the BMP directly, and
            // a surrogate pair folded back into one code point. Anything else
            // becomes '?' rather than a broken byte sequence.
            unsigned int code = hex4(s, i + 1);
            i += 4;
            if (code >= 0xD800 && code <= 0xDBFF && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
              unsigned int low = hex4(s, i + 3);
              i += 6;
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
            appendUtf8(out, code);
            break;
          }
          default: out += s[i];
        }
        i++;
      } else {
        out += s[i++];
      }
    }
    if (i < s.size()) i++;  // closing quote
    return out;
  }

  static unsigned int hex4(const std::string& s, size_t at) {
    unsigned int value = 0;
    for (size_t k = at; k < at + 4 && k < s.size(); k++) {
      char c = s[k];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
    }
    return value;
  }

  static void appendUtf8(std::string& out, unsigned int code) {
    if (code < 0x80) {
      out += static_cast<char>(code);
    } else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  static Json parseNumber(const std::string& s, size_t& i) {
    size_t start = i;
    while (i < s.size() && (isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                            s[i] == 'e' || s[i] == 'E')) {
      i++;
    }
    Json v;
    v.type = Type::Number;
    v.string = s.substr(start, i - start);
    v.number = v.string.empty() ? 0 : atof(v.string.c_str());
    return v;
  }

  static Json parseArray(const std::string& s, size_t& i) {
    Json v;
    v.type = Type::Array;
    i++;  // [
    skip(s, i);
    if (i < s.size() && s[i] == ']') {
      i++;
      return v;
    }
    while (i < s.size()) {
      v.array.push_back(parseValue(s, i));
      skip(s, i);
      if (i < s.size() && s[i] == ',') {
        i++;
        continue;
      }
      if (i < s.size() && s[i] == ']') i++;
      break;
    }
    return v;
  }

  static Json parseObject(const std::string& s, size_t& i) {
    Json v;
    v.type = Type::Object;
    i++;  // {
    skip(s, i);
    if (i < s.size() && s[i] == '}') {
      i++;
      return v;
    }
    while (i < s.size()) {
      skip(s, i);
      std::string key = parseString(s, i);
      skip(s, i);
      if (i < s.size() && s[i] == ':') i++;
      v.object[key] = parseValue(s, i);
      skip(s, i);
      if (i < s.size() && s[i] == ',') {
        i++;
        continue;
      }
      if (i < s.size() && s[i] == '}') i++;
      break;
    }
    return v;
  }
};

}  // namespace tmx
