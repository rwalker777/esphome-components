#include "protocol_goldenmate.h"
#include "protocol_factory.h"
#include "esphome/core/log.h"
#include <cstring>
#include <cstdlib>

namespace esphome {
namespace ups_hid {

static const char *const GM_TAG = "ups_hid.goldenmate";

// Register for vendor 0x075D with a UNIQUE macro name identifier
REGISTER_UPS_PROTOCOL_FOR_VENDOR(
    0x075D, GoldenMateProtocol_075D, 
    [](UpsHidComponent *p) { return std::make_unique<GoldenMateProtocol>(p); },
    "GoldenMate", "GoldenMate UPS Pro", 200);

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
  ESP_LOGD(GM_TAG, "Detecting GoldenMate UPS for 0x075D...");

  // Check if Report 0x0C exists and is long enough
  HidReport megatec;
  if (!read_feature_report(REPORT_ID_MEGATEC, megatec)) {
    ESP_LOGW(GM_TAG, "Failed to read Report 0x0C");
    return false;
  }

  if (megatec.data.size() < 62) {
    ESP_LOGW(GM_TAG, "Report 0x0C too short: %zu bytes", megatec.data.size());
    return false;
  }

  ESP_LOGI(GM_TAG, "GoldenMate UPS protocol successfully detected!");
  return true;
}

bool GoldenMateProtocol::initialize() {
  ESP_LOGD(GM_TAG, "Initializing GoldenMate UPS protocol");

  UpsData data;
  data.device.usb_vendor_id = parent_->get_vendor_id();
  data.device.usb_product_id = parent_->get_product_id();

  // Hardcode manufacturer
  data.device.manufacturer = "GoldenMate";

  // Try to get model from descriptor 2, fallback to "UPS Pro"
  std::string str;
  if (parent_->get_string_descriptor(2, str) == ESP_OK && !str.empty()) {
    data.device.model = str;
  } else {
    data.device.model = "UPS Pro";
  }

  // Get Serial Number from descriptor 3
  if (parent_->get_string_descriptor(3, str) == ESP_OK && !str.empty()) {
    data.device.serial_number = str;
  }

  data.device.capabilities.supports_hid_get_report = true;
  data.device.capabilities.supports_runtime_estimation = true;

  ESP_LOGI(GM_TAG, "GoldenMate UPS initialized: %s %s (S/N: %s)",
           data.device.manufacturer.c_str(),
           data.device.model.c_str(),
           data.device.serial_number.c_str());
           
  return true;
}

bool GoldenMateProtocol::parse_binary_status(const HidReport &report, UpsData &data) {
  if (report.data.size() < 21) {
    return false;
  }

  const uint8_t *d = report.data.data();

  uint8_t battery_pct = d[11];
  if (battery_pct <= 100) {
    data.battery.level = static_cast<float>(battery_pct);
  }

  uint16_t runtime_seconds = d[12] | (d[13] << 8);
  if (runtime_seconds > 0 && runtime_seconds < 65535) {
    data.battery.runtime_minutes = static_cast<float>(runtime_seconds) / 60.0f;
  }

  return true;
}

bool GoldenMateProtocol::parse_megatec_string(const HidReport &report, UpsData &data) {
  if (report.data.size() < 62) {
    return false;
  }

  const uint8_t *d = report.data.data();
  std::string packed;
  for (int i = 30; i < 62; i++) {
    if (d[i] >= '0' && d[i] <= '9') {
      packed += static_cast<char>(d[i]);
    }
  }

  if (packed.size() < 30) {
    ESP_LOGD(GM_TAG, "Packed string too short: %zu chars", packed.size());
    return false;
  }

  // Field 1: Current
  float current = std::atof(packed.substr(0, 4).c_str()) / 100.0f;
  data.power.load_percent = current;

  // Field 2: Voltage
  float voltage = std::atof(packed.substr(4, 4).c_str()) / 100.0f;
  data.battery.voltage = voltage;

  // Field 3: Power
  float power = std::atof(packed.substr(8, 4).c_str()) / 10.0f;
  data.power.input_voltage = power;

  // Field 4: SOC %
  float soc = std::atof(packed.substr(12, 3).c_str());
  data.battery.level = soc;

  // Field 5: Capacity Ah
  float capacity = std::atof(packed.substr(15, 3).c_str()) / 10.0f;
  data.power.frequency = capacity;

  // Field 6: Cycles
  float cycles = std::atof(packed.substr(18, 3).c_str());
  data.power.output_voltage = cycles;

  // Field 7: Temperature
  float temp = std::atof(packed.substr(21, 3).c_str()) / 10.0f;
  data.power.input_transfer_low = temp;

  // Set power status
  data.power.status = "Online";
  if (soc >= 100.0f) {
      data.battery.status = "Full";
  } else if (current > 0.0f) {
      data.battery.status = "Charging";
  } else {
      data.battery.status = "Discharging";
  }

  ESP_LOGI(GM_TAG, "Decoded Data: %.2fV | %.2fA | %.1fW | %.0f%% | %.1fAh | %.0f Cycles | %.1fC", 
           voltage, current, power, soc, capacity, cycles, temp);

  return true;
}

bool GoldenMateProtocol::read_data(UpsData &data) {
  bool success = false;

  HidReport status_report;
  if (read_feature_report(REPORT_ID_STATUS, status_report)) {
    if (parse_binary_status(status_report, data)) {
      success = true;
    }
  }

  HidReport megatec_report;
  if (read_feature_report(REPORT_ID_MEGATEC, megatec_report)) {
    if (parse_megatec_string(megatec_report, data)) {
      success = true;
    }
  }

  // Ensure device info populates sensors properly
  data.device.manufacturer = "GoldenMate";
  std::string str;
  if (parent_->get_string_descriptor(2, str) == ESP_OK && !str.empty()) {
    data.device.model = str;
  } else {
    data.device.model = "UPS Pro";
  }
  if (parent_->get_string_descriptor(3, str) == ESP_OK && !str.empty()) {
    data.device.serial_number = str;
  }

  return success;
}

}  // namespace ups_hid
}  // namespace esphome