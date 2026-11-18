// Debounce.h - filters that keep a noisy input from flipping the station's state. No Arduino dependency.
//
// Both look at the LEVEL of the input on every call, not at edges. A missed edge (an interrupt that
// fired mid-bounce, a reading taken at the wrong moment) can't leave them stuck in the wrong state:
// the next readings put them right.

#pragma once

#include <stdint.h>

// Time-based: a new level is accepted once the input has held it for onMs (going true) or offMs (going false).
class CloudDebounce {
 public:
  bool stable() const { return stable_; }

  // Call with the raw input as often as you like. True when the accepted level just changed.
  bool update(bool raw, uint32_t nowMs, uint32_t onMs, uint32_t offMs) {
    if (!started_ || raw != last_) {
      started_ = true;
      last_ = raw;
      since_ = nowMs;
    }
    if (raw == stable_ || nowMs - since_ < (raw ? onMs : offMs)) return false;
    stable_ = raw;
    return true;
  }

  // Undo the last accepted change (when acting on it failed), so it is accepted again on the next call.
  void revert() { stable_ = !stable_; }

 private:
  bool started_ = false, last_ = false, stable_ = false;
  uint32_t since_ = 0;
};

// Count-based, for checks that run seldom (a camera ping every 10 s): a new level is accepted after
// `confirm` readings in a row agree.
class CloudConfirm {
 public:
  bool stable() const { return stable_; }

  bool update(bool raw, uint8_t confirm) {
    if (raw == stable_) {
      count_ = 0;
      return false;
    }
    if (++count_ < (confirm ? confirm : 1)) return false;
    stable_ = raw;
    count_ = 0;
    return true;
  }

  void revert() { stable_ = !stable_; }

 private:
  bool stable_ = false;
  uint8_t count_ = 0;
};
