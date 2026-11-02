#pragma once

#include "hid_power_device.h"
#include "data_composite.h"
#include "constants_ups.h"
#include <limits>
#include <set>

namespace esphome {
namespace ups_hid {

// Descriptor-based monitoring and limited standard controls.
// IDs/offsets/widths are never device identity.
class Tripplite3024 {
 public:
  static constexpr uint16_t VENDOR_ID = 0x09ae, PRODUCT_ID = 0x3024;
  static bool matches(uint16_t vendor, uint16_t product) { return vendor == VENDOR_ID && product == PRODUCT_ID; }
  hid_power::Descriptor descriptor;

  enum class Command { BEEPER_ENABLE, BEEPER_DISABLE, BEEPER_MUTE, TEST_QUICK, TEST_CANCEL };

  // Only these standard commands are writable. The descriptor must declare an
  // isolated, writable byte Feature. Shared/packed reports fail closed rather
  // than risking a write to an unrelated control in the same report.
  template <typename Reader, typename Writer>
  bool command(Command command, Reader reader, Writer writer) const {
    using namespace hid_power;
    uint32_t usage, collection;
    uint8_t value;
    switch (command) {
      case Command::BEEPER_ENABLE:
        usage = AUDIBLE_ALARM;
        collection = SUMMARY;
        value = beeper::CONTROL_ENABLE;
        break;
      case Command::BEEPER_DISABLE:
        usage = AUDIBLE_ALARM;
        collection = SUMMARY;
        value = beeper::CONTROL_DISABLE;
        break;
      case Command::BEEPER_MUTE:
        usage = AUDIBLE_ALARM;
        collection = SUMMARY;
        value = beeper::CONTROL_MUTE;
        break;
      case Command::TEST_QUICK:
        usage = TEST;
        collection = BATTERY_SYSTEM;
        value = test::COMMAND_QUICK;
        break;
      case Command::TEST_CANCEL:
        usage = TEST;
        collection = BATTERY_SYSTEM;
        value = test::COMMAND_ABORT;
        break;
      default:
        return false;
    }
    const Field *target = nullptr;
    for (const auto &f : descriptor.fields) {
      if (f.type != ReportType::FEATURE || f.usage != usage || !f.has(UPS) || !f.has(collection)) {
        continue;
      }
      if (target) {
        return false;
      }
      target = &f;
    }
    if (!target || (target->flags & 0x107) != 2 || !target->id || target->offset != 0 || target->size != 8 ||
        target->unit != 0 || target->exponent != 0 || value < target->logical_min || value > target->logical_max ||
        descriptor.report_length(*target) != 2) {
      return false;
    }
    for (const auto &f : descriptor.fields) {
      if (&f != target && f.type == ReportType::FEATURE && f.id == target->id) {
        return false;
      }
    }
    uint8_t bytes[2]{};
    size_t length = sizeof(bytes);
    int64_t current;
    if (!reader(3, target->id, bytes, &length) || !descriptor.raw_value(*target, bytes, length, current)) {
      return false;
    }
    bytes[1] = value;
    return writer(3, target->id, bytes, sizeof(bytes));
  }

  // Read each advertised vendor Feature report once. Unknown payloads retain
  // their usage/ID labels, not invented semantics. No Output or Input requests.
  template <typename Reader, typename Log>
  void vendor_diagnostics(Reader reader, Log log) const {
    std::set<uint8_t> seen;
    for (const auto &f : descriptor.fields) {
      if (f.type != hid_power::ReportType::FEATURE || !f.has(hid_power::UPS) || (f.usage >> 16) != 0xffff ||
          !seen.insert(f.id).second) {
        continue;
      }
      size_t length = descriptor.report_length(f);
      std::vector<uint8_t> bytes(length);
      bool ok = reader(3, f.id, bytes.data(), &length) && length == bytes.size() && (!f.id || bytes[0] == f.id);
      log(f.id, f.usage, ok ? bytes : std::vector<uint8_t>{});
    }
  }

  // Reader receives the descriptor's exact type, ID, and wire length. It returns
  // false for stalls/timeouts and replaces length with the actual byte count.
  template <typename Reader>
  bool read(Reader reader, UpsData &data) const {
    using namespace hid_power;
    data.hid_status.descriptor_based = true;
    struct Report {
      bool ok;
      std::vector<uint8_t> bytes;
    };
    std::map<uint16_t, Report> reports;
    auto get = [&](const Field &f, int64_t &raw) {
      uint16_t key = (uint16_t(f.type) << 8) | f.id;
      auto it = reports.find(key);
      if (it == reports.end()) {
        size_t length = descriptor.report_length(f);
        Report report{false, std::vector<uint8_t>(length)};
        report.ok = reader(uint8_t(f.type), f.id, report.bytes.data(), &length);
        if (length != report.bytes.size()) {
          report.ok = false;
        }
        it = reports.emplace(key, std::move(report)).first;
      }
      return it->second.ok && descriptor.raw_value(f, it->second.bytes.data(), it->second.bytes.size(), raw);
    };
    // Prefer the PowerSummary input over converter input, and Feature over Input.
    // Multiple equal-priority fields are ambiguous (for example several outlets).
    auto value = [&](uint32_t usage, uint32_t collection, bool summary_only) -> std::optional<double> {
      for (int summary = 1; summary >= (summary_only ? 1 : 0); --summary) {
        for (auto type : {ReportType::FEATURE, ReportType::INPUT}) {
          const Field *candidate = nullptr;
          size_t matches = 0;
          for (const auto &f : descriptor.fields) {
            if (!f.has(UPS) || f.usage != usage || f.type != type || (collection && !f.has(collection)) ||
                f.has(SUMMARY) != bool(summary)) {
              continue;
            }
            candidate = &f;
            ++matches;
          }
          if (matches > 1) {
            return {};
          }
          if (!candidate) {
            continue;
          }
          const Field &f = *candidate;
          int64_t raw;
          if (!get(f, raw)) {
            continue;
          }
          if (usage == VOLTAGE || usage == CONFIG_VOLTAGE || usage == TRANSFER_LOW || usage == TRANSFER_HIGH) {
            if (f.unit != UNIT_VOLT) {
              continue;
            }
          } else if (usage == RUNTIME) {
            if (f.unit != UNIT_SECOND) {
              continue;
            }
          } else if (usage == ACTIVE_POWER) {
            if (f.unit != UNIT_WATT) {
              continue;
            }
          } else if (usage == CONFIG_VA) {
            if (f.unit != UNIT_WATT) {
              continue;  // PDC watts and VA share a unit encoding.
            }
          } else if (usage == FREQUENCY || usage == CONFIG_FREQUENCY) {
            if (f.unit != UNIT_HERTZ) {
              continue;
            }
          } else if (usage == CURRENT) {
            if (f.unit != UNIT_AMPERE) {
              continue;
            }
          } else if (usage == DELAY_SHUTDOWN || usage == DELAY_START || usage == DELAY_REBOOT ||
                     usage == TL_DELAY_START) {
            if (f.unit != UNIT_SECOND) {
              continue;
            }
            // Some firmwares declare an unsigned countdown. All-ones still
            // encodes PDC's -1 (no countdown), before any physical conversion.
            if (raw == -1 || (f.logical_min >= 0 && raw == ((int64_t(1) << f.size) - 1))) {
              return -1.0;
            }
          } else if (f.unit != 0 || f.exponent != 0) {
            continue;
          }
          double result = Descriptor::physical_value(f, raw);
          // NUT tripplite-hid.c smart1500lcdt_scale(): 09ae:3024 firmware
          // declares live voltage 10^5 too small. Restrict the workaround to
          // its known unit/exponent signature; configuration volts are correct.
          if (usage == VOLTAGE && f.unit == UNIT_VOLT && f.exponent == 1 && f.physical_min == f.physical_max) {
            result *= 100000.0;
          }
          // NUT's same 3024 handler uses 0.01 for live frequency/current. Apply
          // only to this firmware's unit/exponent signature, not nominal values
          // or descriptors that already provide correct physical scaling.
          if ((usage == FREQUENCY || usage == CURRENT) && f.exponent == 1 && f.physical_min == f.physical_max) {
            result *= 0.01;
          }
          return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
        }
      }
      return {};
    };
    bool success = false;
    auto flag = [&](uint32_t usage, std::optional<bool> &dest, uint32_t collection = 0, bool summary_only = true) {
      auto v = value(usage, collection, summary_only);
      if (v && (*v == 0 || *v == 1)) {
        dest = *v != 0;
        success = true;
      }
    };
    auto &s = data.hid_status;
    flag(AC_PRESENT, s.ac_present);
    flag(CHARGING, s.charging);
    flag(DISCHARGING, s.discharging);
    flag(LOW, s.low);
    flag(FULL, s.full);
    flag(EMPTY, s.empty);
    flag(REPLACE, s.replace);
    flag(INTERNAL_FAILURE, s.internal_failure);
    flag(SHUTDOWN_IMMINENT, s.shutdown_imminent);
    if (auto online = s.online()) {
      data.power.status = *online ? status::ONLINE : status::ON_BATTERY;
    }
    if (s.charging.value_or(false) && !s.discharging.value_or(false)) {
      data.battery.status = "Charging";
    } else if (s.discharging.value_or(false) && !s.charging.value_or(false)) {
      data.battery.status = "Discharging";
    } else if (s.full.value_or(false)) {
      data.battery.status = "Fully Charged";
    }
    auto append = [&](const char *text) {
      if (!data.battery.status.empty()) {
        data.battery.status += " / ";
      }
      data.battery.status += text;
    };
    if (s.low.value_or(false)) {
      append("Low");
    }
    if (s.empty.value_or(false)) {
      append("Fully Discharged");
    }
    if (s.replace.value_or(false)) {
      append("Replace Battery");
    }

    auto number = [&](uint32_t usage, uint32_t collection, bool summary, float &dest, double max,
                      double divisor = 1.0) {
      auto v = value(usage, collection, summary);
      if (v && *v >= 0 && *v <= max) {
        dest = *v / divisor;
        success = true;
      }
    };
    // CapacityMode 2 means percent. A small mAh/10mWh value is not a percentage.
    auto mode = value(CAPACITY_MODE, 0, true);
    if (mode && *mode == 2) {
      number(CAPACITY, 0, true, data.battery.level, 100);
      number(LOW_LIMIT, 0, true, data.battery.charge_low, 100);
      number(WARNING_LIMIT, 0, true, data.battery.charge_warning, 100);
    }
    number(RUNTIME, 0, true, data.battery.runtime_minutes, 65534, 60);
    auto load = value(LOAD, OUTPUT, false);
    if (!load) {
      load = value(LOAD, OUTLET, false);
    }
    if (load && *load >= 0 && *load <= 100) {
      data.power.load_percent = *load;
      success = true;
    }
    number(VOLTAGE, INPUT, false, data.power.input_voltage, 300);
    number(VOLTAGE, OUTPUT, false, data.power.output_voltage, 300);
    number(CONFIG_VOLTAGE, INPUT, false, data.power.input_voltage_nominal, 300);
    number(TRANSFER_LOW, OUTPUT, false, data.power.input_transfer_low, 300);
    number(TRANSFER_HIGH, OUTPUT, false, data.power.input_transfer_high, 300);
    number(ACTIVE_POWER, OUTPUT, false, data.power.output_power, std::numeric_limits<float>::max());
    number(VOLTAGE, BATTERY, false, data.battery.voltage, 300);
    number(CONFIG_VOLTAGE, BATTERY, false, data.battery.voltage_nominal, 300);
    if (auto mode = value(AUDIBLE_ALARM, SUMMARY, true)) {
      if (*mode == 1) {
        data.config.parse_beeper_status("disabled");
      } else if (*mode == 2) {
        data.config.parse_beeper_status("enabled");
      } else if (*mode == 3) {
        data.config.parse_beeper_status("muted");
      }
    }
    if (auto result = value(TEST, BATTERY_SYSTEM, false)) {
      static const char *const results[] = {"",
                                            test::RESULT_DONE_PASSED,
                                            test::RESULT_DONE_WARNING,
                                            test::RESULT_DONE_ERROR,
                                            test::RESULT_ABORTED,
                                            test::RESULT_IN_PROGRESS,
                                            test::RESULT_NO_TEST,
                                            test::RESULT_SCHEDULED};
      if (*result >= 1 && *result <= 7 && std::floor(*result) == *result) {
        data.test.ups_test_result = results[static_cast<size_t>(*result)];
      }
    }
    // NUT names these vendor usages; their decimal encoding is left untouched.
    auto version = [&](uint32_t usage, std::string &dest) {
      auto raw = value(usage, 0, false);
      if (raw && *raw >= 0 && *raw <= 65535 && std::floor(*raw) == *raw) {
        dest = std::to_string(static_cast<unsigned>(*raw));
      }
    };
    version(TL_FIRMWARE, data.device.firmware_version);
    version(TL_PROTOCOL, data.device.firmware_aux);
    number(FREQUENCY, INPUT, false, data.power.frequency, 1000);
    number(FREQUENCY, OUTPUT, false, data.power.output_frequency, 1000);
    number(CURRENT, OUTPUT, false, data.power.output_current, std::numeric_limits<float>::max());
    number(CONFIG_VOLTAGE, FLOW, false, data.power.output_voltage_nominal, 300);
    number(CONFIG_FREQUENCY, FLOW, false, data.power.output_frequency_nominal, 1000);
    number(CONFIG_VA, FLOW, false, data.power.apparent_power_nominal, std::numeric_limits<float>::max());
    flag(VOLTAGE_OUT_OF_RANGE, s.voltage_out_of_range, CONVERTER, false);
    flag(OVERLOAD, s.overload, CONVERTER, false);
    flag(BUCK, s.buck, CONVERTER, false);
    flag(BOOST, s.boost, CONVERTER, false);
    flag(OVER_TEMPERATURE, s.over_temperature, CONVERTER, false);
    flag(INTERNAL_FAILURE, s.converter_failure, CONVERTER, false);
    flag(AWAITING_POWER, s.awaiting_power, CONVERTER, false);
    auto timer = [&](std::optional<double> seconds, std::optional<int32_t> &dest) {
      if (seconds && *seconds >= -1 && *seconds <= std::numeric_limits<int32_t>::max() &&
          std::floor(*seconds) == *seconds) {
        dest = static_cast<int32_t>(*seconds);
      }
    };
    timer(value(DELAY_SHUTDOWN, OUTLET, false), s.shutdown_seconds);
    timer(value(DELAY_REBOOT, OUTLET, false), s.reboot_seconds);
    auto startup = value(DELAY_START, OUTLET, false);
    if (!startup) {
      startup = value(TL_DELAY_START, OUTLET, false);
    }
    // The real descriptor maps this vendor field's minutes to physical seconds.
    timer(startup, s.start_seconds);
    // NUT defines the vendor watchdog in seconds: zero means disabled.
    timer(value(TL_WATCHDOG, OUTLET, false), s.watchdog_seconds);
    return success;
  }
};

}  // namespace ups_hid
}  // namespace esphome
