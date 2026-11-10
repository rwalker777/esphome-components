#pragma once
#include <cmath>
#include <string>
namespace esphome::sensor {
class Sensor {
 public:
  float state{NAN};
  void publish_state(float value) { state = value; }
};
}  // namespace esphome::sensor
