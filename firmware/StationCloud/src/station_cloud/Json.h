// Json.h - a tiny JSON writer into a fixed buffer: no heap, never overruns, reports truncation.
// No Arduino dependency.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace cloud_detail {

class Json {
 public:
  Json(char *out, size_t cap) : p_(out), cap_(cap) {
    if (cap_) p_[0] = '\0';
  }
  bool ok() const { return ok_; }        // false = the buffer was too small
  size_t length() const { return n_; }

  // '{' for an object, '[' for an array.
  void open(char bracket = '{') {
    comma();
    char s[2] = {bracket, 0};
    put(s);
    if (depth_ + 1 < kMaxDepth) first_[++depth_] = true;
    else ok_ = false;
  }
  void close(char bracket = '}') {
    char s[2] = {bracket, 0};
    put(s);
    depth_--;
  }
  void key(const char *k) {
    comma();
    put("\"");
    put(k);
    put("\":");
    first_[depth_] = true;  // the value that follows needs no comma
  }
  void str(const char *s) {
    comma();
    if (!s) return put("null");
    put("\"");
    for (; *s; s++) {
      char c = *s;
      if (c == '"' || c == '\\') {
        char e[3] = {'\\', c, 0};
        put(e);
      } else if ((unsigned char)c < 0x20) {
        char e[7];
        snprintf(e, sizeof e, "\\u%04x", (unsigned)c);
        put(e);
      } else {
        char e[2] = {c, 0};
        put(e);
      }
    }
    put("\"");
  }
  void boolean(bool b) {
    comma();
    put(b ? "true" : "false");
  }
  void null() {
    comma();
    put("null");
  }
  void integer(long long v) {
    comma();
    char b[24];
    snprintf(b, sizeof b, "%lld", v);
    put(b);
  }
  // Up to 2 decimals, trailing zeros cut: 1029.6, 64, 28.15. NAN -> null.
  void number(double v) {
    if (!isfinite(v)) return null();
    comma();
    char b[32];
    snprintf(b, sizeof b, "%.2f", v);
    char *dot = strchr(b, '.');
    if (dot) {
      char *e = b + strlen(b) - 1;
      while (e > dot && *e == '0') *e-- = '\0';
      if (e == dot) *e = '\0';
    }
    if (strcmp(b, "-0") == 0) strcpy(b, "0");
    put(b);
  }
  // Out of [lo, hi] or not finite -> null. asInt: send a whole number.
  void ranged(double v, double lo, double hi, bool asInt) {
    if (!isfinite(v) || v < lo || v > hi) return null();
    if (asInt) return integer((long long)llround(v));
    number(v);
  }

 private:
  void comma() {
    if (depth_ < 0) return;
    if (!first_[depth_]) put(",");
    first_[depth_] = false;
  }
  void put(const char *s) {
    size_t k = strlen(s);
    if (n_ + k + 1 > cap_) {
      ok_ = false;
      return;
    }
    memcpy(p_ + n_, s, k + 1);
    n_ += k;
  }

  static constexpr int kMaxDepth = 6;
  char *p_;
  size_t cap_, n_ = 0;
  bool ok_ = true;
  int depth_ = -1;
  bool first_[kMaxDepth] = {true, true, true, true, true, true};
};

}  // namespace cloud_detail
