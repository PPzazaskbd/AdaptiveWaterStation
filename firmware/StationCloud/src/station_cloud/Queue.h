// Queue.h - the buffer of records waiting for AWS (communication-plan.md section 4.5). No Arduino dependency.
//
// Oldest first. A record leaves only after AWS's PUBACK (popFront).
// When full: drop the oldest record without an event. If every record has an event, refuse the new one.

#pragma once

#include <stdint.h>

#include "Record.h"

class CloudQueue {
 public:
  void init(CloudRecord *slots, uint16_t capacity) {
    slots_ = slots;
    cap_ = capacity;
    head_ = count_ = 0;
    dropped_ = 0;
  }
  uint16_t count() const { return count_; }
  uint16_t capacity() const { return cap_; }
  uint32_t dropped() const { return dropped_; }               // plain records dropped, not yet reported
  void forgetDropped(uint32_t n) { dropped_ = n > dropped_ ? 0 : dropped_ - n; }  // after the report arrived

  CloudRecord *front() { return count_ ? &at(0) : nullptr; }
  CloudRecord &at(uint16_t i) { return slots_[(head_ + i) % cap_]; }

  void popFront() {
    if (!count_) return;
    head_ = (head_ + 1) % cap_;
    count_--;
  }

  // false = full of event records: nothing could be dropped, the record was not stored.
  bool pushBack(const CloudRecord &r) {
    if (!makeRoom()) return false;
    at(count_) = r;
    count_++;
    return true;
  }

  // Used for the fresh heartbeat after a reconnect, so the station shows online at once.
  bool pushFront(const CloudRecord &r) {
    if (!makeRoom()) return false;
    head_ = (head_ + cap_ - 1) % cap_;
    count_++;
    at(0) = r;
    return true;
  }

 private:
  bool makeRoom() {
    if (!cap_) return false;
    if (count_ < cap_) return true;
    for (uint16_t i = 0; i < count_; i++) {
      if (at(i).event.type != CLOUD_EV_NONE) continue;
      for (uint16_t j = i; j + 1 < count_; j++) at(j) = at(j + 1);  // close the gap, keep the order
      count_--;
      dropped_++;
      return true;
    }
    return false;
  }

  CloudRecord *slots_ = nullptr;
  uint16_t cap_ = 0, head_ = 0, count_ = 0;
  uint32_t dropped_ = 0;
};
