#pragma once

#include "data_composite.h"
#include "constants_ups.h"
#include <optional>

namespace esphome {
namespace ups_hid {

inline std::optional<bool> online_state(const UpsData &data) {
  if (data.hid_status.descriptor_based) {
    return data.hid_status.online();
  }
  if (data.power.status == status::ONLINE) {
    return true;
  }
  if (data.power.status == status::ON_BATTERY) {
    return false;
  }
  if (data.power.input_voltage_valid()) {
    return true;
  }
  return {};
}

inline std::optional<bool> low_battery_state(const UpsData &data) {
  if (data.hid_status.descriptor_based) {
    return data.hid_status.low;
  }
  if (std::isnan(data.battery.level) || std::isnan(data.battery.charge_low)) {
    return {};
  }
  return data.battery.is_low();
}

inline std::optional<bool> charging_state(const UpsData &data) {
  if (data.hid_status.descriptor_based) {
    return data.hid_status.charging;
  }
  if (!data.battery.is_valid()) {
    return {};
  }
  return data.power.input_voltage_valid() && !std::isnan(data.battery.level) && data.battery.level < 100.0f;
}

inline std::optional<bool> fault_state(const UpsData &data) {
  if (data.hid_status.descriptor_based) {
    return data.hid_status.fault();
  }
  return data.power.is_input_out_of_range() || (!data.power.is_valid() && !data.battery.is_valid());
}

inline std::optional<bool> binary_state(const UpsData &data, const std::string &type) {
  const auto &s = data.hid_status;
  if (type == binary_sensor_type::ONLINE) {
    return online_state(data);
  }
  if (type == binary_sensor_type::ON_BATTERY) {
    auto online = online_state(data);
    return online ? std::optional<bool>(!*online) : std::nullopt;
  }
  if (type == binary_sensor_type::LOW_BATTERY) {
    return low_battery_state(data);
  }
  if (type == binary_sensor_type::CHARGING) {
    return charging_state(data);
  }
  if (type == binary_sensor_type::FAULT) {
    return fault_state(data);
  }
  if (type == binary_sensor_type::OVERLOAD) {
    if (s.descriptor_based) {
      return s.overload;
    }
    if (!std::isnan(data.power.load_percent)) {
      return data.power.is_overloaded();
    }
  }
  if (type == binary_sensor_type::VOLTAGE_OUT_OF_RANGE) {
    return s.voltage_out_of_range;
  }
  if (type == binary_sensor_type::BUCK) {
    return s.buck;
  }
  if (type == binary_sensor_type::BOOST) {
    return s.boost;
  }
  if (type == binary_sensor_type::OVER_TEMPERATURE) {
    return s.over_temperature;
  }
  if (type == binary_sensor_type::CONVERTER_FAILURE) {
    return s.converter_failure;
  }
  if (type == binary_sensor_type::AWAITING_POWER) {
    return s.awaiting_power;
  }
  if (type == binary_sensor_type::DISCHARGING) {
    return s.discharging;
  }
  if (type == binary_sensor_type::INTERNAL_FAILURE) {
    return s.internal_failure;
  }
  if (type == binary_sensor_type::SHUTDOWN_IMMINENT) {
    return s.shutdown_imminent;
  }
  if (type == binary_sensor_type::REPLACE_BATTERY) {
    return s.replace;
  }
  if (type == binary_sensor_type::BATTERY_FULL) {
    return s.full;
  }
  if (type == binary_sensor_type::BATTERY_EMPTY) {
    return s.empty;
  }
  return {};
}

inline std::optional<int32_t> timer_state(const UpsData &data, const std::string &type) {
  const auto &s = data.hid_status;
  if (s.descriptor_based) {
    if (type == sensor_type::UPS_TIMER_SHUTDOWN) {
      return s.shutdown_seconds;
    }
    if (type == sensor_type::UPS_TIMER_START) {
      return s.start_seconds;
    }
    if (type == sensor_type::UPS_TIMER_REBOOT) {
      return s.reboot_seconds;
    }
    if (type == sensor_type::UPS_WATCHDOG) {
      return s.watchdog_seconds;
    }
    return {};
  }
  if (type == sensor_type::UPS_TIMER_SHUTDOWN && data.test.timer_shutdown != -1) {
    return data.test.timer_shutdown;
  }
  if (type == sensor_type::UPS_TIMER_START && data.test.timer_start != -1) {
    return data.test.timer_start;
  }
  if (type == sensor_type::UPS_TIMER_REBOOT && data.test.timer_reboot != -1) {
    return data.test.timer_reboot;
  }
  return {};
}

}  // namespace ups_hid
}  // namespace esphome
