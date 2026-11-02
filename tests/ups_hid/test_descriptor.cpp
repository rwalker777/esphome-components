#include "../../components/ups_hid/tripplite_3024.h"
#include "../../components/ups_hid/state_helpers.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <random>
#include <set>

using namespace esphome::ups_hid;
using namespace esphome::ups_hid::hid_power;

// Synthetic descriptor based on USB HID/PDC usages and NUT's AVR900U field
// listing. This is NOT a dump from the user's INTERNET350U.
struct Fixture {
  std::vector<uint8_t> bytes{0x05, 0x84, 0x09, 0x04, 0xa1, 0x01};
  std::map<uint16_t, std::vector<uint8_t>> reports;
  void add(std::initializer_list<uint8_t> b) { bytes.insert(bytes.end(), b); }
  void collection(uint8_t usage) { add({0x05, 0x84, 0x09, usage, 0xa1, 0x00}); }
  void end() { add({0xc0}); }
  void field(uint8_t id, uint8_t page, uint8_t usage, uint8_t bits, uint32_t max, uint32_t unit = 0,
             uint8_t exponent = 0, uint8_t main = 0xb1) {
    add({0x85, id, 0x05, page, 0x09, usage, 0x15, 0x00, 0x27});
    for (int i = 0; i < 4; ++i) {
      bytes.push_back(max >> (8 * i));
    }
    add({0x35, 0x00, 0x45, 0x00, 0x67});
    for (int i = 0; i < 4; ++i) {
      bytes.push_back(unit >> (8 * i));
    }
    add({0x55, exponent, 0x75, bits, 0x95, 0x01, main, 0x02});
  }
  Fixture(uint8_t status_id = 0x32, uint8_t capacity_id = 0x34, uint8_t voltage_exponent = 1) {
    collection(0x24);  // PowerSummary
    collection(0x1a);  // Input
    field(0x30, 0x84, 0x40, 8, 255, UNIT_VOLT, 7);
    field(0x31, 0x84, 0x30, 16, 65535, UNIT_VOLT, voltage_exponent);
    end();
    collection(0x02);  // PresentStatus
    add({0x85, status_id, 0x75, 0x01, 0x95, 0x07, 0xb1, 0x03});
    field(status_id, 0x84, 0x62, 1, 1);  // InternalFailure bit 7
    add({0x75, 0x01, 0x95, 0x01, 0xb1, 0x03});
    field(status_id, 0x84, 0x69, 1, 1);  // ShutdownImminent bit 9
    add({0x75, 0x01, 0x95, 0x06, 0xb1, 0x03});
    field(status_id, 0x85, 0xd0, 1, 1);         // AC present bit 16
    add({0x75, 0x01, 0x95, 0x01, 0xb1, 0x03});  // padding bit 17
    for (uint8_t usage : {0x42, 0x46, 0x44, 0x45, 0x47, 0x4b}) {
      field(status_id, 0x85, usage, 1, 1);
    }
    end();
    field(0x33, 0x85, 0x2c, 8, 2);
    field(capacity_id, 0x85, 0x66, 8, 100);
    field(0x35, 0x85, 0x68, 16, 65535, UNIT_SECOND);
    field(0x38, 0x85, 0x8c, 8, 100);
    end();
    collection(0x16);
    collection(0x1c);  // PowerConverter.Output
    field(0x1b, 0x84, 0x30, 16, 65535, UNIT_VOLT, voltage_exponent);

    field(0x06, 0x84, 0x53, 16, 300, UNIT_VOLT, 7);
    end();
    end();
    collection(0x18);
    collection(0x20);  // OutletSystem.Outlet, as in the NUT capture
    field(0x1e, 0x84, 0x35, 8, 100);
    end();
    end();
    end();
    reports = {{uint16_t(0x300 | status_id), {status_id, 0, 0, 0x19}},  // AC+charging+full
               {0x333, {0x33, 2}},
               {uint16_t(0x300 | capacity_id), {capacity_id, 94}},
               {0x335, {0x35, 0x42, 0x0e}},
               {0x338, {0x38, 30}},
               {0x330, {0x30, 120}},
               {0x331, {0x31, 0xb7, 0x04}},
               {0x31b, {0x1b, 0xc1, 0x04}},
               {0x31e, {0x1e, 0}},
               {0x306, {0x06, 95, 0}}};
  }
  bool read(Tripplite3024 &decoder, UpsData &data) {
    data.reset();
    std::set<uint16_t> read_once;
    return decoder.read(
        [&](uint8_t type, uint8_t id, uint8_t *buffer, size_t *length) {
          uint16_t key = (type << 8) | id;
          assert(read_once.insert(key).second);  // one request per type+ID per cycle
          auto it = reports.find(key);
          if (it == reports.end()) {
            return false;
          }
          assert(*length >= it->second.size());
          std::copy(it->second.begin(), it->second.end(), buffer);
          *length = it->second.size();
          return true;
        },
        data);
  }
};

void test_device_read() {
  Fixture f;
  Tripplite3024 decoder;
  assert(decoder.descriptor.parse(f.bytes.data(), f.bytes.size()));
  UpsData d;
  assert(f.read(decoder, d));
  assert(d.power.status == status::ONLINE && d.hid_status.online() == true);
  assert(std::abs(d.power.input_voltage - 120.7f) < 0.01f);
  assert(std::abs(d.power.output_voltage - 121.7f) < 0.01f);
  assert(d.power.input_voltage_nominal == 120 && d.power.input_transfer_low == 95);
  assert(d.power.load_percent == 0 && d.battery.level == 94);
  assert(std::abs(d.battery.runtime_minutes - 3650.f / 60) < 0.01f);
  assert(d.battery.charge_warning == 30 && std::isnan(d.battery.charge_low));
  assert(d.hid_status.low == false && d.hid_status.replace == false);
  f.reports[0x332] = {0x32, 0, 0, 0x20};
  assert(f.read(decoder, d));
  assert(d.power.status == status::ON_BATTERY && d.hid_status.online() == false);
  assert(d.battery.status == "Discharging");
  f.reports.erase(0x332);
  assert(f.read(decoder, d));
  assert(!d.hid_status.online().has_value() && d.power.status.empty());
  f.reports[0x332] = {0x32, 0, 0};      // short report
  f.reports[0x334] = {0x34, 101};       // invalid percentage
  f.reports[0x335] = {0x35, 255, 255};  // unknown runtime
  assert(f.read(decoder, d));
  assert(!d.hid_status.low && std::isnan(d.battery.level) && std::isnan(d.battery.runtime_minutes));
  f.reports[0x334] = {0x34, 50};
  f.reports[0x333] = {0x33, 0};  // capacity is mAh, not percent
  assert(f.read(decoder, d) && std::isnan(d.battery.level));
  f.reports.clear();
  assert(!f.read(decoder, d) && !d.hid_status.online());
  assert(std::isnan(d.power.input_voltage) && d.battery.status.empty());
}

void test_input_fallback_and_invalid_identity() {
  Fixture f;
  f.add({0x05, 0x84, 0x09, 0x04, 0xa1, 0x01});
  f.collection(0x24);
  f.collection(0x02);
  // Input and Feature can have different layouts despite an equal report ID.
  f.field(0x32, 0x85, 0xd0, 1, 1, 0, 0, 0x81);
  f.field(0x32, 0x85, 0x45, 1, 1, 0, 0, 0x81);
  f.end();
  f.end();
  f.end();
  Tripplite3024 decoder;
  assert(decoder.descriptor.parse(f.bytes.data(), f.bytes.size()));
  f.reports.erase(0x332);
  f.reports[0x132] = {0x32, 1};
  UpsData d;
  assert(f.read(decoder, d) && d.hid_status.online() == true);
  assert(!d.hid_status.charging.has_value());
  f.reports.erase(0x132);
  f.reports[0x332] = {0x99, 0, 0, 0x19};
  assert(f.read(decoder, d) && !d.hid_status.online().has_value());
}

void test_units_and_duplicate_paths() {
  Fixture f(0x32, 0x34, 6);  // Correct descriptor: tenths of volts need no vendor factor.
  Tripplite3024 decoder;
  assert(decoder.descriptor.parse(f.bytes.data(), f.bytes.size()));
  UpsData d;
  assert(f.read(decoder, d) && std::abs(d.power.input_voltage - 120.7f) < 0.01f);
  f.reports[0x331] = {0x31, 0xb6, 0x03};  // raw 950 is 95V, including during brownout.
  assert(f.read(decoder, d) && std::abs(d.power.input_voltage - 95.0f) < 0.01f);
  f.add({5, 0x84, 9, 4, 0xa1, 1});
  f.collection(0x24);
  f.collection(0x1a);
  f.field(0x73, 0x84, 0x30, 16, 65535, UNIT_VOLT, 6);
  f.end();
  f.end();
  f.end();
  assert(decoder.descriptor.parse(f.bytes.data(), f.bytes.size()));
  assert(f.read(decoder, d) && std::isnan(d.power.input_voltage));
  assert(!std::isnan(d.power.output_voltage));
}

void test_ids_are_not_semantics() {
  Fixture f(0x71, 0x72);
  Tripplite3024 decoder;
  assert(decoder.descriptor.parse(f.bytes.data(), f.bytes.size()));
  UpsData d;
  assert(f.read(decoder, d) && d.power.status == status::ONLINE && d.battery.level == 94);
}

std::vector<uint8_t> hardware_descriptor_bytes() {
  std::ifstream input("tests/ups_hid/fixtures/internet350u-3024-descriptor.hex");
  assert(input.is_open());
  std::vector<uint8_t> bytes;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream row(line);
    unsigned byte;
    while (row >> std::hex >> byte) {
      assert(byte <= 255);
      bytes.push_back(byte);
    }
  }
  assert(bytes.size() == 885);
  return bytes;
}

void test_hardware_descriptor() {
  auto bytes = hardware_descriptor_bytes();
  Tripplite3024 decoder;
  assert(decoder.descriptor.parse(bytes.data(), bytes.size()));
  auto field = [&](uint32_t usage, uint32_t collection) -> const Field & {
    const Field *found = nullptr;
    for (const auto &f : decoder.descriptor.fields) {
      if (f.type != ReportType::FEATURE || f.usage != usage || !f.has(collection)) {
        continue;
      }
      assert(found == nullptr);
      found = &f;
    }
    assert(found != nullptr);
    return *found;
  };
  const auto &ac = field(AC_PRESENT, SUMMARY);
  assert(ac.id == 0x32 && ac.offset == 16 && ac.size == 1 && decoder.descriptor.report_length(ac) == 4);
  const auto &charging = field(CHARGING, SUMMARY);
  assert(charging.id == 0x32 && charging.offset == 20);
  const auto &capacity_mode = field(CAPACITY_MODE, SUMMARY);
  assert(capacity_mode.id == 0x33 && capacity_mode.offset == 0 && capacity_mode.size == 8);
  const auto &nominal = field(CONFIG_VOLTAGE, INPUT);
  assert(nominal.id == 0x30 && nominal.exponent == 7);
  const auto &input_voltage = field(VOLTAGE, SUMMARY);
  assert(input_voltage.id == 0x31 && input_voltage.unit == UNIT_VOLT && input_voltage.exponent == 1);
  const auto &runtime = field(RUNTIME, SUMMARY);
  assert(runtime.id == 0x35 && runtime.size == 16 && runtime.unit == UNIT_SECOND);
  const auto &load = field(LOAD, OUTLET);
  assert(load.id == 0x1e && load.size == 8);
  assert(field(WARNING_LIMIT, SUMMARY).id == 0x38);
  const auto &power = field(ACTIVE_POWER, OUTPUT);
  assert(power.id == 0x47 && power.size == 16 && power.unit == UNIT_WATT && power.exponent == 7);
  const auto &battery_voltage = field(VOLTAGE, BATTERY);
  assert(battery_voltage.id == 0x20 && battery_voltage.size == 16 && battery_voltage.exponent == 1);
  const auto &battery_nominal = field(CONFIG_VOLTAGE, BATTERY);
  assert(battery_nominal.id == 0x04 && battery_nominal.size == 16 && battery_nominal.exponent == 7);
  // Exercise realistic report values against the REAL descriptor. These status,
  // capacity and runtime payloads are test inputs, not captured device readings.
  Fixture reports;
  UpsData d;
  assert(reports.read(decoder, d));
  assert(d.hid_status.online() == true && d.hid_status.low == false);
  assert(d.battery.level == 94 && std::abs(d.battery.runtime_minutes - 3650.f / 60) < 0.01f);
  assert(d.power.input_voltage_nominal == 120 && std::abs(d.power.input_voltage - 120.7f) < 0.01f);
  assert(d.battery.charge_warning == 30 && std::isnan(d.battery.charge_low));
  assert(std::isnan(d.power.output_power));  // optional report absent from this modeled sample
  reports.reports[0x347] = {0x47, 55, 0};    // modeled 55 W, independent of percent load
  assert(reports.read(decoder, d) && d.power.output_power == 55);
  reports.reports[0x347] = {0x47, 0, 0};
  assert(reports.read(decoder, d) && d.power.output_power == 0);
  reports.reports[0x347] = {0x47, 55};  // short read must clear previous watts
  assert(reports.read(decoder, d) && std::isnan(d.power.output_power));
  reports.reports.erase(0x347);  // a stalled optional power read leaves other values available
  assert(reports.read(decoder, d) && std::isnan(d.power.output_power) && d.battery.level == 94);
  // These battery payloads are modeled; captured readings are covered separately.
  // AC voltage must not fill this sensor; nominal volts must not get the live-voltage fix.
  assert(std::isnan(d.battery.voltage) && std::isnan(d.battery.voltage_nominal));
  reports.reports[0x320] = {0x20, 135, 0};
  reports.reports[0x304] = {0x04, 12, 0};
  assert(reports.read(decoder, d) && std::abs(d.battery.voltage - 13.5f) < 0.001f);
  assert(d.battery.voltage_nominal == 12 && std::abs(d.power.input_voltage - 120.7f) < 0.01f);
  reports.reports[0x320] = {0x20, 135};  // short response clears the previous reading
  assert(reports.read(decoder, d) && std::isnan(d.battery.voltage) && d.battery.voltage_nominal == 12);
  reports.reports.erase(0x320);
  reports.reports.erase(0x304);
  assert(reports.read(decoder, d) && std::isnan(d.battery.voltage) && std::isnan(d.battery.voltage_nominal));
  // Control status and version values are modeled; no USB write is performed.
  reports.reports[0x311] = {0x11, 3};
  reports.reports[0x310] = {0x10, 5};
  reports.reports[0x30f] = {0x0f, 42};
  reports.reports[0x36c] = {0x6c, 0x00, 0x0a};
  assert(reports.read(decoder, d) && d.config.beeper_status == "muted");
  assert(d.test.ups_test_result == "In progress");
  assert(d.device.firmware_version == "42" && d.device.firmware_aux == "2560");
  reports.reports[0x311] = {0x11, 0};
  reports.reports[0x310] = {0x10, 0};
  reports.reports.erase(0x30f);
  reports.reports.erase(0x36c);
  assert(reports.read(decoder, d) && d.config.beeper_status.empty() && d.test.ups_test_result.empty());
  assert(d.device.firmware_version.empty() && d.device.firmware_aux.empty());
  reports.reports[0x332] = {0x32, 0, 0, 0x20};
  assert(reports.read(decoder, d) && d.power.status == status::ON_BATTERY);
}

void test_fault_report_completeness() {
  Tripplite3024 decoder;
  auto bytes = hardware_descriptor_bytes();
  assert(decoder.descriptor.parse(bytes.data(), bytes.size()));
  Fixture reports;
  reports.reports[0x322] = {0x22, 0, 0};
  UpsData d;
  assert(reports.read(decoder, d) && binary_state(d, "fault") == false);

  reports.reports.erase(0x322);
  assert(reports.read(decoder, d));
  assert(d.hid_status.internal_failure == false && d.hid_status.shutdown_imminent == false);
  assert(!binary_state(d, "fault").has_value());

  reports.reports[0x322] = {0x22, 0};
  assert(reports.read(decoder, d) && !binary_state(d, "fault").has_value());
  reports.reports[0x322] = {0x22, 0xd5, 0x40};
  assert(reports.read(decoder, d) && binary_state(d, "fault") == true);

  reports.reports.erase(0x332);
  assert(reports.read(decoder, d) && binary_state(d, "fault") == true);
  reports.reports[0x322] = {0x22, 0, 0};
  assert(reports.read(decoder, d) && !binary_state(d, "fault").has_value());
}

void test_descriptor_commands_and_diagnostics() {
  Tripplite3024 decoder;
  auto bytes = hardware_descriptor_bytes();
  assert(decoder.descriptor.parse(bytes.data(), bytes.size()));
  using Command = Tripplite3024::Command;
  struct Expected {
    Command command;
    uint8_t id, value;
  };
  int writes = 0;
  auto reader = [](uint8_t type, uint8_t id, uint8_t *report, size_t *length) {
    assert(type == 3 && *length == 2);
    report[0] = id;
    report[1] = id == 0x10 ? 6 : 2;
    *length = 2;
    return true;
  };
  for (auto expected : {Expected{Command::BEEPER_ENABLE, 0x11, 2},
                        {Command::BEEPER_DISABLE, 0x11, 1},
                        {Command::BEEPER_MUTE, 0x11, 3},
                        {Command::TEST_QUICK, 0x10, 1},
                        {Command::TEST_CANCEL, 0x10, 3}}) {
    assert(
        decoder.command(expected.command, reader, [&](uint8_t type, uint8_t id, const uint8_t *report, size_t length) {
          ++writes;
          assert(type == 3 && id == expected.id && length == 2 && report[0] == id && report[1] == expected.value);
          return true;
        }));
  }
  assert(writes == 5);
  auto no_write = [](uint8_t, uint8_t, const uint8_t *, size_t) {
    assert(false);
    return false;
  };
  // Read failure, wrong ID and truncation must prevent any write.
  for (int failure : {0, 1, 2}) {
    assert(!decoder.command(
        Command::BEEPER_MUTE,
        [failure](uint8_t, uint8_t id, uint8_t *report, size_t *length) {
          report[0] = failure == 1 ? 0xff : id;
          report[1] = 2;
          if (failure == 2) {
            *length = 1;
          }
          return failure != 0;
        },
        no_write));
  }
  int failed_writes = 0;
  assert(!decoder.command(Command::TEST_QUICK, reader, [&](uint8_t, uint8_t, const uint8_t *, size_t) {
    ++failed_writes;
    return false;
  }) && failed_writes == 1);  // An uncertain write outcome is never retried.
  assert(!decoder.command(static_cast<Command>(99), reader, no_write));

  Field *alarm = nullptr;
  for (auto &f : decoder.descriptor.fields) {
    if (f.usage == AUDIBLE_ALARM) {
      alarm = &f;
    }
  }
  assert(alarm && alarm->flags == 0x82);
  Field original = *alarm;
  for (int invalid : {0, 1, 2, 3, 4, 5}) {
    *alarm = original;
    if (invalid == 0) {
      alarm->flags |= 1;  // Constant Feature is read-only.
    }
    if (invalid == 1) {
      alarm->offset = 1;
    }
    if (invalid == 2) {
      alarm->unit = UNIT_SECOND;
    }
    if (invalid == 3) {
      alarm->logical_max = 1;
    }
    if (invalid == 4) {
      alarm->size = 7;
    }
    if (invalid == 5) {
      alarm->flags |= 0x100;  // Buffered bytes are not scalar commands.
    }
    assert(!decoder.command(Command::BEEPER_MUTE, reader, no_write));
  }
  *alarm = original;
  // A second named field sharing a report is not safe for a byte command.
  auto extra = original;
  extra.usage = 0xffff0092;
  decoder.descriptor.fields.push_back(extra);
  assert(!decoder.command(Command::BEEPER_MUTE, reader, no_write));
  decoder.descriptor.fields.pop_back();
  extra = original;
  extra.id = 0x70;
  decoder.descriptor.fields.push_back(extra);
  assert(!decoder.command(Command::BEEPER_MUTE, reader, no_write));  // ambiguous paths
  decoder.descriptor.fields.pop_back();

  std::set<uint8_t> seen;
  size_t logs = 0;
  decoder.vendor_diagnostics(
      [&](uint8_t type, uint8_t id, uint8_t *report, size_t *length) {
        assert(type == 3 && seen.insert(id).second);
        report[0] = id;
        // One vendor response fails; other advertised reports still get examined.
        return id != 0x0f && *length >= 2;
      },
      [&](uint8_t id, uint32_t usage, const std::vector<uint8_t> &report) {
        ++logs;
        assert((usage >> 16) == 0xffff && ((id == 0x0f) == report.empty()));
      });
  assert(logs == seen.size() && seen.count(0x52) && seen.count(0x6c) && seen.count(0xe3));

  // The same command at another descriptor-selected ID still works.
  Fixture moved;
  moved.add({5, 0x84, 9, 4, 0xa1, 1});
  moved.collection(0x24);
  moved.field(0x71, 0x84, 0x5a, 8, 3);
  moved.end();
  moved.end();
  assert(decoder.descriptor.parse(moved.bytes.data(), moved.bytes.size()));
  assert(
      decoder.command(Command::BEEPER_MUTE, reader, [](uint8_t type, uint8_t id, const uint8_t *report, size_t length) {
        assert(type == 3 && id == 0x71 && length == 2 && report[0] == 0x71 && report[1] == 3);
        return true;
      }));
}

void test_parser_boundaries() {
  Descriptor d;
  // Unnumbered, two packed fields, signed minimum and negative exponent.
  const uint8_t packed[] = {5,    0x84, 9,    4,    0xa1, 1,    0x15, 0xf8, 0x25, 7, 0x75, 4,
                            0x95, 2,    0x19, 0x30, 0x29, 0x31, 0x55, 0x0f, 0x81, 2, 0xc0};
  assert(d.parse(packed, sizeof(packed)) && d.fields.size() == 2);
  const uint8_t raw[] = {0x7e};
  int64_t v;
  assert(d.raw_value(d.fields[0], raw, 1, v) && v == -2);
  assert(std::abs(Descriptor::physical_value(d.fields[0], v) + 0.2) < 1e-8);
  assert(d.raw_value(d.fields[1], raw, 1, v) && v == 7);
  assert(!d.raw_value(d.fields[0], raw, 0, v));
  Field physical = d.fields[1];
  physical.physical_min = 0;
  physical.physical_max = 150;
  physical.exponent = 0;
  assert(std::abs(Descriptor::physical_value(physical, 7) - 150) < 1e-8);
  // PDC watts use centimetres and grams: exponent 7 is whole watts.
  Field watts = d.fields[1];
  watts.unit = 0x0000d121;
  watts.exponent = 7;
  assert(Descriptor::physical_value(watts, 55) == 55);
  watts.exponent = 6;
  assert(std::abs(Descriptor::physical_value(watts, 55) - 5.5) < 1e-8);
  Fixture f;
  for (size_t len = 0; len < f.bytes.size(); ++len) {
    // This fixture has one outer collection, closed only at its final byte.
    assert(!d.parse(f.bytes.data(), len) && d.fields.empty());
  }
  const uint8_t malformed[] = {0x05};
  assert(!d.parse(malformed, sizeof(malformed)) && d.fields.empty());
  const uint8_t pop[] = {0xb4};
  assert(!d.parse(pop, sizeof(pop)));
  const uint8_t long_item[] = {0xfe, 0xff, 0};
  assert(!d.parse(long_item, sizeof(long_item)));
  // Push/pop restores ReportID and offsets remain separate for Input/Feature.
  const uint8_t scopes[] = {5, 0x84, 9,    4, 0xa1, 1,    0x15, 0, 0x25, 100, 0x75, 8,    0x95, 1,   0x85,
                            1, 0xa4, 0x85, 2, 9,    0x35, 0x81, 2, 0xb4, 9,   0x35, 0xb1, 2,    0xc0};
  assert(d.parse(scopes, sizeof(scopes)) && d.fields.size() == 2);
  assert(d.fields[0].id == 2 && d.fields[1].id == 1);
  assert(d.fields[0].offset == 0 && d.fields[1].offset == 0);
  std::mt19937 random(3024);
  for (size_t n = 0; n < 20000; ++n) {
    std::vector<uint8_t> noise(random() % 256);
    for (auto &b : noise) {
      b = random();
    }
    d.parse(noise.data(), noise.size());
  }
}

void test_output_power_units_and_paths() {
  // IDs are incidental. This descriptor reports tenths of watts at a new ID,
  // alongside input active power and output apparent power that must be ignored.
  for (auto unit : {UNIT_WATT, UNIT_VOLT}) {
    Fixture f;
    f.add({5, 0x84, 9, 4, 0xa1, 1});
    f.collection(0x1a);
    f.field(0x70, 0x84, 0x34, 16, 65535, UNIT_WATT, 7);
    f.end();
    f.collection(0x1c);
    f.field(0x71, 0x84, 0x33, 16, 65535, UNIT_WATT, 7);
    f.field(0x72, 0x84, 0x34, 16, 65535, unit, 6);
    f.end();
    f.end();
    f.reports[0x370] = {0x70, 99, 0};
    f.reports[0x371] = {0x71, 88, 0};
    f.reports[0x372] = {0x72, 123, 0};
    Tripplite3024 decoder;
    assert(decoder.descriptor.parse(f.bytes.data(), f.bytes.size()));
    UpsData d;
    assert(f.read(decoder, d));
    if (unit == UNIT_WATT) {
      assert(std::abs(d.power.output_power - 12.3f) < 0.001f);
    } else {
      assert(std::isnan(d.power.output_power));
    }
  }
}

void test_status_unknown() {
  HidStatus s;
  assert(!s.online().has_value() && !s.fault().has_value());
  s.ac_present = true;
  s.discharging = true;
  assert(s.online() == true);  // A self-test can discharge while AC remains present.
  s.ac_present = false;
  s.discharging = false;
  assert(!s.online().has_value());
  s.internal_failure = false;
  s.shutdown_imminent = false;
  assert(!s.fault().has_value());
  s.converter_failure = false;
  s.over_temperature = false;
  s.overload = false;
  assert(s.fault() == false);
  s.shutdown_imminent = true;
  assert(s.fault() == true);
}

void test_captured_self_test_and_beeper() {
  auto descriptor = hardware_descriptor_bytes();
  Tripplite3024 decoder;
  assert(decoder.descriptor.parse(descriptor.data(), descriptor.size()));
  std::ifstream input("tests/ups_hid/fixtures/internet350u-self-test-reports.txt");
  assert(input.is_open());
  struct Expected {
    const char *beeper, *test;
    float battery_voltage, watts;
    bool discharging;
  };
  const Expected expected[] = {
      {"muted", "No test initiated", 13.3f, 32, false},   {"disabled", "No test initiated", 13.3f, 32, false},
      {"enabled", "No test initiated", 13.3f, 34, false}, {"enabled", "In progress", 12.2f, 44, true},
      {"enabled", "Done and passed", 12.8f, 30, true},    {"enabled", "Done and passed", 13.0f, 32, false},
  };
  std::string line;
  size_t sample = 0;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    assert(sample < sizeof(expected) / sizeof(expected[0]));
    Fixture reports;
    reports.reports.clear();
    std::istringstream row(line);
    std::string part;
    std::getline(row, part, '|');  // capture timestamp
    while (std::getline(row, part, '|')) {
      std::istringstream hex(part);
      unsigned byte;
      std::vector<uint8_t> bytes;
      while (hex >> std::hex >> byte) {
        assert(byte <= 255);
        bytes.push_back(byte);
      }
      assert(!bytes.empty());
      reports.reports[0x300 | bytes[0]] = bytes;
    }
    UpsData d;
    assert(reports.read(decoder, d));
    const auto &e = expected[sample++];
    // Mains availability and battery discharge are independent during a test.
    assert(d.power.status == status::ONLINE && binary_state(d, "online") == true);
    assert(binary_state(d, "on_battery") == false);
    assert(d.hid_status.discharging == e.discharging);
    assert(d.battery.status == (e.discharging ? "Discharging" : "Charging"));
    assert(d.config.beeper_status == e.beeper && d.test.ups_test_result == e.test);
    assert(std::abs(d.battery.voltage - e.battery_voltage) < 0.001f && d.battery.voltage_nominal == 12);
    assert(d.power.output_power == e.watts);
    assert(d.device.firmware_version == "161" && d.device.firmware_aux == "12324");
  }
  assert(sample == sizeof(expected) / sizeof(expected[0]));
}

void test_read_only_diagnostics() {
  Tripplite3024 decoder;
  auto bytes = hardware_descriptor_bytes();
  assert(decoder.descriptor.parse(bytes.data(), bytes.size()));
  Fixture reports;  // Modeled values against the hardware descriptor; not live measurements.
  reports.reports[0x319] = {0x19, 0x57, 0x02};  // 59.9 Hz, known 3024 unit/exponent correction
  reports.reports[0x31c] = {0x1c, 0x59, 0x02};  // 60.1 Hz
  reports.reports[0x346] = {0x46, 3, 0};        // 0.3 A
  reports.reports[0x301] = {0x01, 120};
  reports.reports[0x330] = {0x30, 117};  // distinct from nominal output voltage
  reports.reports[0x302] = {0x02, 60};
  reports.reports[0x303] = {0x03, 0x5e, 1};     // 350 VA
  reports.reports[0x322] = {0x22, 0xd5, 0x40};  // includes AwaitingPower at payload bit 14
  UpsData d;
  assert(reports.read(decoder, d));
  assert(std::abs(d.power.frequency - 59.9f) < 0.001f);
  assert(std::abs(d.power.output_frequency - 60.1f) < 0.001f);
  assert(std::abs(d.power.output_current - 0.3f) < 0.001f);
  assert(d.power.output_voltage_nominal == 120 && d.power.output_frequency_nominal == 60);
  assert(d.power.apparent_power_nominal == 350);
  assert(d.hid_status.voltage_out_of_range == true && d.hid_status.buck == false);
  assert(d.hid_status.boost == true && d.hid_status.overload == true);
  assert(d.hid_status.over_temperature == true && d.hid_status.converter_failure == true);
  assert(d.hid_status.awaiting_power == true);
  assert(binary_state(d, "fault") == true);  // converter faults must not be hidden by clear summary flags
  reports.reports[0x315] = {0x15, 45, 0};
  reports.reports[0x361] = {0x61, 0xff, 0xff};  // known idle, not missing
  reports.reports[0x317] = {0x17, 0, 0x80};     // 32768 seconds must not overflow an int16_t
  reports.reports[0x352] = {0x52, 0};           // disabled watchdog
  reports.reports[0x316] = {0x16, 2, 0};        // must not override a known idle standard startup timer
  assert(reports.read(decoder, d));
  assert(d.hid_status.shutdown_seconds == 45 && d.hid_status.start_seconds == -1);
  assert(d.hid_status.reboot_seconds == 32768 && d.hid_status.watchdog_seconds == 0);
  assert(timer_state(d, sensor_type::UPS_TIMER_START) == -1);
  assert(timer_state(d, sensor_type::UPS_TIMER_REBOOT) == 32768);
  assert(timer_state(d, sensor_type::UPS_WATCHDOG) == 0);
  reports.reports.erase(0x361);  // vendor fallback's physical range maps minutes to seconds
  reports.reports[0x315] = {0x15, 0xff, 0xff};
  reports.reports[0x317] = {0x17, 0xfe, 0xff};
  reports.reports[0x352] = {0x52, 60};
  assert(reports.read(decoder, d));
  assert(d.hid_status.start_seconds == 120 && d.hid_status.shutdown_seconds == -1);
  assert(d.hid_status.reboot_seconds == 65534 && d.hid_status.watchdog_seconds == 60);
  reports.reports[0x316] = {0x16, 0xff, 0xff};  // signed idle survives the minutes-to-seconds conversion
  assert(reports.read(decoder, d) && d.hid_status.start_seconds == -1);

  assert(binary_state(d, "voltage_out_of_range") == true && binary_state(d, "boost") == true);
  assert(binary_state(d, "buck") == false && binary_state(d, "overload") == true);
  assert(binary_state(d, "over_temperature") == true && binary_state(d, "converter_failure") == true);
  assert(binary_state(d, "awaiting_power") == true && binary_state(d, "battery_full") == true);
  reports.reports[0x322] = {0x22, 0, 0};
  reports.reports[0x31e] = {0x1e, 99};
  assert(reports.read(decoder, d) && binary_state(d, "overload") == false);  // no guessed load threshold
  assert(binary_state(d, "fault") == false);

  // A corrected descriptor must not receive the vendor scale factor twice.
  auto original = decoder.descriptor;
  for (auto &f : decoder.descriptor.fields) {
    if (f.usage == FREQUENCY || f.usage == CURRENT) {
      f.exponent = -1;
    }
  }
  assert(reports.read(decoder, d));
  assert(std::abs(d.power.frequency - 59.9f) < 0.001f && std::abs(d.power.output_current - 0.3f) < 0.001f);
  decoder.descriptor = original;
  for (auto &f : decoder.descriptor.fields) {
    if (f.usage == CURRENT) {
      f.physical_max = f.logical_max;
    }
  }
  assert(reports.read(decoder, d) && d.power.output_current == 30);  // explicit physical range is respected
  decoder.descriptor = original;
  for (auto &f : decoder.descriptor.fields) {
    if (f.usage == FREQUENCY || f.usage == CURRENT) {
      f.unit = UNIT_VOLT;
    }
  }
  assert(reports.read(decoder, d));
  assert(std::isnan(d.power.frequency) && std::isnan(d.power.output_current));
  decoder.descriptor = original;
  for (uint16_t id : {0x319, 0x31c, 0x346, 0x303, 0x322, 0x315, 0x317, 0x316, 0x352}) {
    reports.reports.erase(id);
  }
  assert(reports.read(decoder, d));
  assert(std::isnan(d.power.frequency) && std::isnan(d.power.apparent_power_nominal));
  assert(!binary_state(d, "overload") && !binary_state(d, "awaiting_power"));
  assert(!timer_state(d, sensor_type::UPS_TIMER_START) && !timer_state(d, sensor_type::UPS_WATCHDOG));
}

void test_published_state_resolution() {
  UpsData d;
  d.hid_status.descriptor_based = true;
  d.power.input_voltage = 120.7f;
  // Plausible voltage never supplies AC state for this descriptor path.
  assert(!binary_state(d, "online").has_value() && !binary_state(d, "on_battery").has_value());
  assert(!binary_state(d, "low_battery").has_value() && !binary_state(d, "charging").has_value());
  d.hid_status.ac_present = false;
  d.hid_status.discharging = true;
  d.hid_status.charging = false;
  d.hid_status.low = true;
  assert(binary_state(d, "online") == false && binary_state(d, "on_battery") == true);
  assert(binary_state(d, "charging") == false && binary_state(d, "low_battery") == true);
  d.hid_status.low.reset();
  d.battery.level = 40;
  d.battery.charge_warning = 50;
  assert(!binary_state(d, "low_battery").has_value());
  d.battery.charge_low = 20;
  assert(!binary_state(d, "low_battery").has_value());
  d.hid_status = HidStatus{};
  assert(binary_state(d, "low_battery") == false);  // Legacy protocols retain the capacity fallback.
  d.power.status = status::ON_BATTERY;
  assert(binary_state(d, "online") == false && binary_state(d, "on_battery") == true);
}

void test_config_descriptor() {
  const uint8_t config[] = {9, 2, 34,   0,    1, 1, 0, 0x80, 50,   9, 4, 3, 0,    1, 3,  0, 0,
                            0, 9, 0x21, 0x11, 1, 0, 1, 0x22, 0x75, 3, 7, 5, 0x81, 3, 64, 0, 255};
  assert(report_descriptor_length(config, sizeof(config), 3) == 885);
  assert(report_descriptor_length(config, sizeof(config), 0) == 0);
  assert(report_descriptor_length(config, 20, 3) == 0);
}

int main() {
  assert(Tripplite3024::matches(0x09ae, 0x3024));
  assert(!Tripplite3024::matches(0x09ae, 0x2009));
  assert(!Tripplite3024::matches(0x09ae, 0x2012));
  assert(!Tripplite3024::matches(0x051d, 0x3024));
  test_device_read();
  test_ids_are_not_semantics();
  test_parser_boundaries();
  test_input_fallback_and_invalid_identity();
  test_units_and_duplicate_paths();
  test_hardware_descriptor();
  test_fault_report_completeness();
  test_output_power_units_and_paths();
  test_descriptor_commands_and_diagnostics();
  test_captured_self_test_and_beeper();
  test_read_only_diagnostics();
  test_status_unknown();
  test_config_descriptor();
  test_published_state_resolution();
  std::cout << "HID descriptor/decoder regression checks passed\n";
}
