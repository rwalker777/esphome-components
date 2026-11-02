#pragma once
#include <cstdint>
namespace esphome {
namespace setup_priority {
constexpr float DATA = 600;
}
class PollingComponent {
 public:
  virtual ~PollingComponent() = default;
  virtual void setup() {}
  virtual void update() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }
  void mark_failed() { failed_ = true; }
  bool is_failed() const { return failed_; }
  void status_set_warning() { warning_ = true; }
  void status_clear_warning() { warning_ = false; }
  bool status_has_warning() const { return warning_; }
  uint32_t get_update_interval() const { return 5000; }
  template<typename Callback> void set_timeout(const char *, uint32_t, Callback) {}

 private:
  bool failed_{false};
  bool warning_{false};
};
}  // namespace esphome
