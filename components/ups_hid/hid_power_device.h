#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace esphome {
namespace ups_hid {
namespace hid_power {

// USB HID 1.11 short items; Power Device (0x84) and Battery System (0x85) usages.
constexpr uint32_t UPS = 0x840004, SUMMARY = 0x840024, INPUT = 0x84001a, OUTPUT = 0x84001c;
constexpr uint32_t OUTLET = 0x840020;
constexpr uint32_t BATTERY = 0x840012;
constexpr uint32_t BATTERY_SYSTEM = 0x840010, TEST = 0x840058, AUDIBLE_ALARM = 0x84005a;
constexpr uint32_t CONVERTER = 0x840016, FLOW = 0x84001e;
constexpr uint32_t FREQUENCY = 0x840032, CURRENT = 0x840031;
constexpr uint32_t CONFIG_FREQUENCY = 0x840042, CONFIG_VA = 0x840043;
constexpr uint32_t VOLTAGE_OUT_OF_RANGE = 0x840063, OVERLOAD = 0x840065;
constexpr uint32_t BUCK = 0x84006f, BOOST = 0x84006e, OVER_TEMPERATURE = 0x840067;
constexpr uint32_t AWAITING_POWER = 0x840072;
constexpr uint32_t DELAY_SHUTDOWN = 0x840057, DELAY_START = 0x840056, DELAY_REBOOT = 0x840055;
constexpr uint32_t TL_DELAY_START = 0xffff0056, TL_WATCHDOG = 0xffff0092;
constexpr uint32_t TL_FIRMWARE = 0xffff007c, TL_PROTOCOL = 0xffff007d;
constexpr uint32_t VOLTAGE = 0x840030, CONFIG_VOLTAGE = 0x840040, LOAD = 0x840035;
constexpr uint32_t ACTIVE_POWER = 0x840034;
constexpr uint32_t TRANSFER_LOW = 0x840053, TRANSFER_HIGH = 0x840054;
constexpr uint32_t CAPACITY = 0x850066, CAPACITY_MODE = 0x85002c, RUNTIME = 0x850068;
constexpr uint32_t LOW_LIMIT = 0x850029, WARNING_LIMIT = 0x85008c;
constexpr uint32_t AC_PRESENT = 0x8500d0, CHARGING = 0x850044, DISCHARGING = 0x850045;
constexpr uint32_t LOW = 0x850042, FULL = 0x850046, EMPTY = 0x850047, REPLACE = 0x85004b;
constexpr uint32_t INTERNAL_FAILURE = 0x840062, SHUTDOWN_IMMINENT = 0x840069;
constexpr uint32_t UNIT_VOLT = 0x00f0d121, UNIT_WATT = 0x0000d121, UNIT_SECOND = 0x00001001;
constexpr uint32_t UNIT_HERTZ = 0x0000f001, UNIT_AMPERE = 0x00100001;
constexpr size_t MAX_DESCRIPTOR_SIZE = 4096, MAX_REPORT_SIZE = 64, MAX_FIELDS = 256, MAX_DEPTH = 16;

enum class ReportType : uint8_t { INPUT = 1, OUTPUT = 2, FEATURE = 3 };

struct Field {
  ReportType type;
  uint8_t id;
  uint16_t offset;
  uint8_t size;
  uint32_t usage;
  std::vector<uint32_t> path;
  int64_t logical_min, logical_max, physical_min, physical_max;
  uint32_t unit;
  int8_t exponent;
  uint32_t flags;
  bool has(uint32_t collection) const { return std::find(path.begin(), path.end(), collection) != path.end(); }
};

class Descriptor {
 public:
  std::vector<Field> fields;

  bool parse(const uint8_t *data, size_t length) {
    fields.clear();
    report_bits_.clear();
    if (data == nullptr || length == 0 || length > MAX_DESCRIPTOR_SIZE) {
      return false;
    }
    if (!parse_(data, length)) {
      fields.clear();
      report_bits_.clear();
      return false;
    }
    return !fields.empty();
  }

  size_t report_length(const Field &f) const {
    auto it = report_bits_.find(key_(f.type, f.id));
    return it == report_bits_.end() ? 0 : (it->second + 7) / 8 + (f.id != 0);
  }

  bool raw_value(const Field &f, const uint8_t *data, size_t length, int64_t &result) const {
    if (!data || length != report_length(f) || (f.id && data[0] != f.id)) {
      return false;
    }
    size_t start = f.offset + (f.id ? 8 : 0);
    if (start + f.size > length * 8) {
      return false;
    }
    uint32_t raw = 0;
    for (size_t bit = 0; bit < f.size; ++bit) {
      raw |= uint32_t((data[(start + bit) / 8] >> ((start + bit) % 8)) & 1) << bit;
    }
    result = raw;
    if (f.logical_min < 0 && (raw & (uint32_t(1) << (f.size - 1)))) {
      result -= int64_t(1) << f.size;
    }
    return result >= f.logical_min && result <= f.logical_max;
  }

  static double physical_value(const Field &f, int64_t raw) {
    double value = static_cast<double>(raw);
    // HID 1.11: an undefined physical range defaults to the logical range.
    if (f.physical_min != f.physical_max && f.logical_max > f.logical_min) {
      value = f.physical_min +
              (value - f.logical_min) * double(f.physical_max - f.physical_min) / double(f.logical_max - f.logical_min);
    }
    // HID SI linear units use centimetres and grams: volts/watts normalize at 7.
    int base_exponent = (f.unit == UNIT_VOLT || f.unit == UNIT_WATT) ? 7 : 0;
    return value * std::pow(10.0, f.exponent - base_exponent);
  }

 private:
  struct Globals {
    uint32_t page{0}, size{0}, count{0}, unit{0};
    uint8_t id{0};
    int64_t lmin{0}, lmax{0}, pmin{0}, pmax{0};
    int8_t exponent{0};
  };
  std::map<uint16_t, size_t> report_bits_;
  static uint16_t key_(ReportType type, uint8_t id) { return (uint16_t(type) << 8) | id; }
  static int64_t signed_(uint32_t value, size_t bytes) {
    if (bytes && (value & (uint32_t(1) << (bytes * 8 - 1)))) {
      return int64_t(value) - (int64_t(1) << (bytes * 8));
    }
    return value;
  }

  bool parse_(const uint8_t *data, size_t length) {
    Globals g;
    std::vector<Globals> stack;
    std::vector<uint32_t> path, usages;
    std::optional<uint32_t> usage_min, usage_max;
    bool numbered = false, unnumbered = false;
    for (size_t pos = 0; pos < length;) {
      uint8_t prefix = data[pos++];
      if (prefix == 0xfe) {
        return false;  // Long items have no supported semantics here.
      }
      size_t bytes = prefix & 3;
      if (bytes == 3) {
        bytes = 4;
      }
      if (bytes > length - pos) {
        return false;
      }
      uint32_t value = 0;
      for (size_t i = 0; i < bytes; ++i) {
        value |= uint32_t(data[pos++]) << (i * 8);
      }
      uint8_t type = (prefix >> 2) & 3, tag = prefix >> 4;
      if (type == 1) {
        switch (tag) {
          case 0:
            if (value > 0xffff) {
              return false;
            }
            g.page = value;
            break;
          case 1:
            g.lmin = signed_(value, bytes);
            break;
          case 2:
            g.lmax = g.lmin < 0 ? signed_(value, bytes) : int64_t(value);
            break;
          case 3:
            g.pmin = signed_(value, bytes);
            break;
          case 4:
            g.pmax = g.pmin < 0 ? signed_(value, bytes) : int64_t(value);
            break;
          case 5:
            g.exponent = (value & 8) ? int(value & 15) - 16 : int(value & 15);
            break;
          case 6:
            g.unit = value;
            break;
          case 7:
            g.size = value;
            break;
          case 8:
            if (!value || value > 255) {
              return false;
            }
            g.id = value;
            numbered = true;
            break;
          case 9:
            g.count = value;
            break;
          case 10:
            if (stack.size() == MAX_DEPTH) {
              return false;
            }
            stack.push_back(g);
            break;
          case 11:
            if (stack.empty()) {
              return false;
            }
            g = stack.back();
            stack.pop_back();
            break;
          default:
            return false;
        }
      } else if (type == 2) {
        uint32_t usage = bytes == 4 ? value : (g.page << 16) | value;
        if (tag == 0) {
          if (usages.size() == MAX_FIELDS) {
            return false;
          }
          usages.push_back(usage);
        } else if (tag == 1) {
          usage_min = usage;
        } else if (tag == 2) {
          usage_max = usage;
        } else if (tag >= 7 && tag <= 9) { /* String index/range metadata does not change field layout. */
        } else {
          return false;  // Delimiter sets and designators are not supported.
        }
      } else if (type == 0) {
        if (usage_min.has_value() != usage_max.has_value()) {
          return false;
        }
        if (usage_min) {
          if (*usage_max < *usage_min || *usage_max - *usage_min >= MAX_FIELDS) {
            return false;
          }
          for (uint32_t u = *usage_min;; ++u) {
            if (usages.size() == MAX_FIELDS) {
              return false;
            }
            usages.push_back(u);
            if (u == *usage_max) {
              break;
            }
          }
        }
        if (tag == 10) {
          if (path.size() == MAX_DEPTH || usages.empty()) {
            return false;
          }
          path.push_back(usages.front());
        } else if (tag == 12) {
          if (path.empty()) {
            return false;
          }
          path.pop_back();
        } else if (tag == 8 || tag == 9 || tag == 11) {
          if (!g.size || g.size > 32 || !g.count || g.count > MAX_FIELDS) {
            return false;
          }
          auto report_type = tag == 8 ? ReportType::INPUT : tag == 9 ? ReportType::OUTPUT : ReportType::FEATURE;
          size_t &offset = report_bits_[key_(report_type, g.id)];
          size_t bits = size_t(g.size) * g.count;
          if (offset + bits + (g.id ? 8 : 0) > MAX_REPORT_SIZE * 8) {
            return false;
          }
          if (!g.id) {
            unnumbered = true;
          }
          if (numbered && unnumbered) {
            return false;
          }
          // Tripp Lite marks named read-only Feature values (e.g. CapacityMode)
          // Constant/Variable. Keep those; unnamed padding and arrays only consume bits.
          if ((value & 6) == 2 && (!(value & 1) || report_type == ReportType::FEATURE)) {
            if (g.lmax < g.lmin || fields.size() + g.count > MAX_FIELDS) {
              return false;
            }
            for (uint32_t i = 0; i < g.count; ++i) {
              if (usages.empty()) {
                continue;
              }
              fields.push_back({report_type, g.id, uint16_t(offset + i * g.size), uint8_t(g.size),
                                usages[std::min(size_t(i), usages.size() - 1)], path, g.lmin, g.lmax, g.pmin, g.pmax,
                                g.unit, g.exponent, value});
            }
          }
          offset += bits;
        } else {
          return false;
        }
        usages.clear();
        usage_min.reset();
        usage_max.reset();
      } else {
        return false;
      }
    }
    return path.empty() && stack.empty() && usages.empty() && !usage_min && !usage_max;
  }
};

// Find the report descriptor length on the claimed HID interface, not an arbitrary interface.
inline size_t report_descriptor_length(const uint8_t *config, size_t length, uint8_t interface_number) {
  bool selected = false;
  for (size_t pos = 0; pos + 2 <= length;) {
    size_t size = config[pos];
    uint8_t type = config[pos + 1];
    if (size < 2 || size > length - pos) {
      return 0;
    }
    if (type == 4) {
      if (size < 9) {
        return 0;
      }
      selected = config[pos + 2] == interface_number && config[pos + 3] == 0 && config[pos + 5] == 3;
    } else if (type == 0x21 && selected) {
      if (size < 6 || size_t(6 + config[pos + 5] * 3) > size) {
        return 0;
      }
      for (size_t i = 6; i + 3 <= size; i += 3) {
        if (config[pos + i] != 0x22) {
          continue;
        }
        size_t result = config[pos + i + 1] | (size_t(config[pos + i + 2]) << 8);
        return result && result <= MAX_DESCRIPTOR_SIZE ? result : 0;
      }
    }
    pos += size;
  }
  return 0;
}

}  // namespace hid_power
}  // namespace ups_hid
}  // namespace esphome
