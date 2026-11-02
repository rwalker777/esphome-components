#pragma once

#include "data_battery.h"
#include <optional>
#include "data_power.h"
#include "data_device.h"
#include "data_test.h"
#include "data_config.h"
#include <cstdint>
#include <cstring>  // For memcmp

namespace esphome {
namespace ups_hid {

struct HidStatus {
  bool descriptor_based{false};
  std::optional<bool> ac_present, charging, discharging, low, full, empty, replace;
  std::optional<bool> internal_failure, shutdown_imminent;
  std::optional<bool> voltage_out_of_range, overload, buck, boost;
  std::optional<bool> over_temperature, converter_failure, awaiting_power;
  // A known -1 countdown is idle; nullopt means no usable report.
  std::optional<int32_t> shutdown_seconds, start_seconds, reboot_seconds, watchdog_seconds;

  std::optional<bool> online() const {
    // A self-test can discharge the battery while AC remains present.
    if (ac_present && *ac_present) {
      return true;
    }
    if (discharging && *discharging) {
      return false;
    }
    return {};
  }

  std::optional<bool> fault() const {
    if (internal_failure.value_or(false) || shutdown_imminent.value_or(false) || converter_failure.value_or(false) ||
        over_temperature.value_or(false) || overload.value_or(false)) {
      return true;
    }
    if (internal_failure && shutdown_imminent && converter_failure && over_temperature && overload) {
      return false;
    }
    return {};
  }
};

/**
 * Clean UPS Composite Data Structure
 * 
 * Contains all UPS data organized into logical components following
 * Single Responsibility Principle. No legacy compatibility code.
 */
struct UpsCompositeData {
  // Specialized data components
  HidStatus hid_status;
  BatteryData battery;
  PowerData power;
  DeviceInfo device;
  TestStatus test;
  ConfigData config;
  
  // Validation and utility methods
  bool is_valid() const {
    return battery.is_valid() || power.is_valid() || device.is_valid() || 
           test.is_valid() || config.is_valid();
  }
  
  bool has_core_data() const {
    return battery.is_valid() && power.is_valid();
  }
  
  // Clean reset without legacy flags
  void reset() {
    hid_status = HidStatus{};
    battery.reset();
    power.reset();
    device.reset();
    test.reset();
    config.reset();
  }
  
  // Copy constructor and assignment for safe copying
  UpsCompositeData() = default;
  UpsCompositeData(const UpsCompositeData&) = default;
  UpsCompositeData& operator=(const UpsCompositeData&) = default;
  UpsCompositeData(UpsCompositeData&&) = default;
  UpsCompositeData& operator=(UpsCompositeData&&) = default;
};

// Type alias for cleaner naming
using UpsData = UpsCompositeData;

}  // namespace ups_hid
}  // namespace esphome
