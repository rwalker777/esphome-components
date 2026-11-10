#pragma once
#include <string>
namespace esphome::binary_sensor {
class BinarySensor {
 public:
  bool state{false};
  bool has_state() const { return valid_; }
  void publish_state(bool value) { state = value; valid_ = true; }
  void invalidate_state() { valid_ = false; }

 private:
  bool valid_{false};
};
}  // namespace esphome::binary_sensor
