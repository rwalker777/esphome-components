#include "protocol_goldenmate.h"
#include "protocol_factory.h"
#include "esphome/core/log.h"
#include <cstring>
#include <cstdlib>

namespace esphome {
namespace ups_hid {

static const char *const GM_TAG = "ups_hid.goldenmate";

// Register for vendor 0x06DA (-BMS- / GoldenMate) with high priority
REGISTER_UPS_PROTOCOL_FOR_VENDOR(
    0x06DA, GoldenMateProtocol,
    [](UpsHidComponent *p) { return std::make_unique<GoldenMateProtocol>(p); },
    "GoldenMate BMS", "GoldenMate / BMS Smart-Battery UPS", 200);

// Register for vendor 0x075D with a UNIQUE macro name identifier
REGISTER_UPS_PROTOCOL_FOR_VENDOR(
    0x075D, GoldenMateProtocol_075D, 
    [](UpsHidComponent *p) { return std::make_unique<GoldenMateProtocol>(p); },
    "GoldenMate UPS", "GoldenMate UPS Pro", 200);

bool GoldenMateProtocol::read_feature_report(uint8_t report_id, HidReport &report) {
  if (!parent_->is_device_connected()) {
    return false;
  }

  uint8_t buffer[64];
  size_t buffer_len = sizeof(buffer);

  esp_err_t ret = parent_->hid_get_report(HID_REPORT_TYPE_FEATURE, report_id,
                                           buffer, &buffer_len,
                                           parent_->get_protocol_timeout());
  if (ret == ESP_OK && buffer_len > 0) {
    report.report_id = report_id;
    report.data.assign(buffer, buffer + buffer_len);
    return true;
  }
  return false;
}

bool GoldenMateProtocol::detect() {
  ESP_LOGD(GM_TAG, "=== START RAW HEX DUMP FOR 0x075D ===");

  // The specific report IDs we saw your UPS answering to
  uint8_t reports_to_check[] = {0x01, 0x06, 0x0C, 0x16, 0x30, 0x31};

  for (uint8_t r_id : reports_to_check) {
    HidReport report;
    if (read_feature_report(r_id, report)) {
      std::string hex_dump = "";
      char buf[5];
      // Loop through every single byte and convert to hex
      for (size_t i = 0; i < report.data.size(); i++) {
        snprintf(buf, sizeof(buf), "%02X ", report.data[i]);
        hex_dump += buf;
      }
      ESP_LOGD(GM_TAG, "Report 0x%02X (%zu bytes): %s", r_id, report.data.size(), hex_dump.c_str());
    } else {
      ESP_LOGD(GM_TAG, "Report 0x%02X: Failed to read", r_id);
    }
  }
  
  ESP_LOGD(GM_TAG, "=== END RAW HEX DUMP ===");

  // Return false so ESPHome doesn't crash trying to parse it yet.
  // It will safely fall back to the Generic HID parser after printing our data.
  return false; 
}

bool GoldenMateProtocol::initialize() {
  ESP_LOGD(GM_TAG, "Initializing GoldenMate BMS protocol");

  // Read device info from USB string descriptors
  UpsData data;
  data.device.usb_vendor_id = parent_->get_vendor_id();
  data.device.usb_product_id = parent_->get_product_id();

  std::string str;
  if (parent_->get_string_descriptor(1, str) == ESP_OK) {
    data.device.manufacturer = str;
  } else {
    data.device.manufacturer = "GoldenMate";
  }

  if (parent_->get_string_descriptor(2, str) == ESP_OK) {
    data.device.model = str;
  }

  if (parent_->get_string_descriptor(3, str) == ESP_OK) {
    data.device.serial_number = str;
  }

  data.device.capabilities.supports_hid_get_report = true;
  data.device.capabilities.supports_runtime_estimation = true;

  ESP_LOGI(GM_TAG, "GoldenMate BMS initialized: %s %s (S/N: %s)",
           data.device.manufacturer.c_str(),
           data.device.model.c_str(),
           data.device.serial_number.c_str());
  return true;
}

std::string GoldenMateProtocol::extract_packed_ascii(const uint8_t *data, size_t offset, size_t len) {
  std::string result;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[offset + i];
    if (b >= '0' && b <= '9') {
      result += static_cast<char>(b);
    }
  }
  return result;
}

bool GoldenMateProtocol::parse_binary_status(const HidReport &report, UpsData &data) {
  if (report.data.size() < 21) {
    return false;
  }

  const uint8_t *d = report.data.data();

  // Battery level (byte 11)
  uint8_t battery_pct = d[11];
  if (battery_pct <= 100) {
    data.battery.level = static_cast<float>(battery_pct);
  }

  // Runtime to empty (bytes 12-13, little-endian, in seconds)
  uint16_t runtime_seconds = d[12] | (d[13] << 8);
  if (runtime_seconds > 0 && runtime_seconds < 65535) {
    data.battery.runtime_minutes = static_cast<float>(runtime_seconds) / 60.0f;
  }

  // Nominal input voltage (byte 16, static config value)
  uint8_t nominal_voltage = d[16];
  if (nominal_voltage > 50 && nominal_voltage < 250) {
    data.power.input_voltage_nominal = static_cast<float>(nominal_voltage);
  }

  // Temperature (byte 20, BMS internal)
  uint8_t temp = d[20];
  if (temp > 0 && temp < 100) {
    // Store as battery temperature via status string
    // (no dedicated temperature field in UpsData)
  }

  ESP_LOGD(GM_TAG, "Binary: battery=%d%% runtime=%ds(%0.1fmin) nominal=%dV temp=%d°C",
           battery_pct, runtime_seconds, data.battery.runtime_minutes,
           nominal_voltage, temp);

  return true;
}

bool GoldenMateProtocol::parse_megatec_string(const HidReport &report, UpsData &data) {
  if (report.data.size() < 62) {
    return false;
  }

  const uint8_t *d = report.data.data();

  // Extract the packed ASCII string from bytes 30-61 (32 chars)
  // Format: LLLLFFFFOOOOBBBBFFFVVVTTTSSSSSSSS
  // Load(4) TransferLow(4) TransferHigh(4) Batt(3) Freq(3) BattV(3) Temp(3) Status(8) = 32

  // Verify we have ASCII digits
  int digit_count = 0;
  for (int i = 30; i < 62; i++) {
    if (d[i] >= '0' && d[i] <= '9') digit_count++;
  }
  if (digit_count < 20) {
    ESP_LOGD(GM_TAG, "Megatec string not ready (only %d digits)", digit_count);
    return false;
  }

  std::string packed;
  for (int i = 30; i < 62; i++) {
    if (d[i] >= '0' && d[i] <= '9') {
      packed += static_cast<char>(d[i]);
    }
  }

  if (packed.size() < 30) {
    ESP_LOGD(GM_TAG, "Packed Megatec string too short: %zu chars", packed.size());
    return false;
  }

  ESP_LOGD(GM_TAG, "Megatec packed: %s", packed.c_str());

  // Parse fields from packed string
  // Field 1: Load % × 10 (4 chars)
  // Note: at no/very low load, this field reads 9999 (999.9%) which is
  // the battery % leaking into this field. Only trust values <= 100%.
  std::string load_str = packed.substr(0, 4);
  float load_raw = std::atof(load_str.c_str()) / 10.0f;
  if (load_raw >= 0.0f && load_raw <= 100.0f) {
    data.power.load_percent = load_raw;
  } else {
    // At no load, field 1 shows battery % — use 0
    data.power.load_percent = 0.0f;
  }

  // Field 2: Low voltage transfer × 10 (4 chars)
  std::string transfer_low_str = packed.substr(4, 4);
  float transfer_low = std::atof(transfer_low_str.c_str()) / 10.0f;
  if (transfer_low > 0.0f && transfer_low < 300.0f) {
    data.power.input_transfer_low = transfer_low;
  }

  // Field 3: High voltage transfer × 10 (4 chars)
  std::string transfer_high_str = packed.substr(8, 4);
  float transfer_high = std::atof(transfer_high_str.c_str()) / 10.0f;
  if (transfer_high > 0.0f && transfer_high < 300.0f) {
    data.power.input_transfer_high = transfer_high;
  }

  // Field 4: Battery % (3 chars)
  std::string batt_str = packed.substr(12, 3);
  float batt_pct = std::atof(batt_str.c_str());
  // Use Report 0x01 battery value preferentially (already set in parse_binary_status)

  // Field 5: Frequency × 10 (3 chars)
  std::string freq_str = packed.substr(15, 3);
  float freq = std::atof(freq_str.c_str()) / 10.0f;
  if (freq >= 45.0f && freq <= 65.0f) {
    data.power.frequency = freq;
  }

  // Field 6: Battery voltage × 100 (3 chars)
  std::string battv_str = packed.substr(18, 3);
  float batt_voltage = std::atof(battv_str.c_str()) / 100.0f;
  if (batt_voltage > 0.0f && batt_voltage < 60.0f) {
    data.battery.voltage = batt_voltage;
  }

  // Field 7: Temperature × 10 (3 chars)
  std::string temp_str = packed.substr(21, 3);
  float temp = std::atof(temp_str.c_str()) / 10.0f;
  // No dedicated temperature field in UpsData, but log it

  // Field 8: Status bits (8 chars)
  std::string status_str;
  if (packed.size() >= 30) {
    status_str = packed.substr(24, 8);
  }

  // Parse status bits
  bool on_battery = false;
  if (status_str.size() >= 7) {
    // Bit 6 (index 1 from left in "01000000") = on battery
    on_battery = (status_str[1] == '1');
  }

  // Set power status
  if (on_battery) {
    data.power.status = "On Battery";
    data.battery.status = "Discharging";
  } else {
    data.power.status = "Online";
    if (batt_pct >= 99.0f) {
      data.battery.status = "Full";
    } else {
      data.battery.status = "Charging";
    }
  }

  ESP_LOGD(GM_TAG, "Megatec: load=%.1f%% freq=%.1fHz battV=%.2fV temp=%.1f°C status=%s on_battery=%s",
           load_raw, freq, batt_voltage, temp,
           status_str.c_str(), on_battery ? "YES" : "no");

  return true;
}

bool GoldenMateProtocol::read_data(UpsData &data) {
  bool success = false;

  // Read binary status from Report 0x01
  HidReport status_report;
  if (read_feature_report(REPORT_ID_STATUS, status_report)) {
    if (parse_binary_status(status_report, data)) {
      success = true;
    }
  }

  // Read Megatec ASCII data from Report 0x0C
  HidReport megatec_report;
  if (read_feature_report(REPORT_ID_MEGATEC, megatec_report)) {
    if (parse_megatec_string(megatec_report, data)) {
      success = true;
    }
  }

  // Set device info
  data.device.usb_vendor_id = parent_->get_vendor_id();
  data.device.usb_product_id = parent_->get_product_id();

  std::string str;
  if (data.device.manufacturer.empty()) {
    if (parent_->get_string_descriptor(1, str) == ESP_OK) {
      data.device.manufacturer = str;
    } else {
      data.device.manufacturer = "GoldenMate";
    }
  }
  if (data.device.model.empty()) {
    if (parent_->get_string_descriptor(2, str) == ESP_OK) {
      data.device.model = str;
    } else {
      data.device.model = "Smart-Battery";
    }
  }
  if (data.device.serial_number.empty()) {
    if (parent_->get_string_descriptor(3, str) == ESP_OK) {
      data.device.serial_number = str;
    }
  }

  // Set apparent power rating (1000VA for the GoldenMate 1000VA Pro)
  // The packed Megatec bytes 18-19 in Report 0x01 = 1320, possibly VA rating
  data.power.apparent_power_nominal = 1000.0f;

  return success;
}

}  // namespace ups_hid
}  // namespace esphome
