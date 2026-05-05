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
    "GoldenMate", "UPS Pro", 200);

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

  HidReport megatec;
  if (!read_feature_report(REPORT_ID_MEGATEC, megatec)) {
    return false;
  }

  if (megatec.data.size() < 62) {
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

  // Set actual names
  data.device.manufacturer = "GoldenMate";
  data.device.model = "UPS Pro";

  // Get Serial Number from descriptor 3
  std::string str;
  if (parent_->get_string_descriptor(3, str) == ESP_OK && !str.empty()) {
    data.device.serial_number = str;
  }

  data.device.capabilities.supports_hid_get_report = true;
  data.device.capabilities.supports_runtime_estimation = true;

  ESP_LOGI(GM_TAG, "UPS initialized: %s %s (S/N: %s)",
           data.device.manufacturer.c_str(),
           data.device.model.c_str(),
           data.device.serial_number.c_str());
           
  return true;
}

bool GoldenMateProtocol::parse_binary_status(const HidReport &report, UpsData &data) {
  // We ignore the binary runtime here to prevent the 16-bit overflow bug
  // The true runtime will be grabbed from the ASCII string instead.
  return true;
}

bool GoldenMateProtocol::parse_megatec_string(const HidReport &report, UpsData &data) {
  if (report.data.size() < 62) return false;

  const uint8_t *d = report.data.data();
  std::string packed;
  
  // Extract exactly bytes 30 to 61
  for (int i = 30; i < 62; i++) {
    char c = static_cast<char>(d[i]);
    if (c >= '0' && c <= '9') {
      packed += c;
    } else if (c == ' ' || c == 0x00) {
      // Normalize spaces/nulls to zeros to keep the 32-character string perfectly aligned
      packed += '0';
    }
  }

  if (packed.size() < 30) {
    ESP_LOGW(GM_TAG, "Report 0x0C buffer was empty or invalid.");
    return false;
  }

  // Extract the live values safely
  float runtime_mins = std::atof(packed.substr(0, 4).c_str());
  float battery_pct  = std::atof(packed.substr(12, 3).c_str());
  
  // --- SANITY CHECK FILTER ---
  if (runtime_mins == 0.0f && battery_pct == 0.0f) {
    ESP_LOGW(GM_TAG, "Transient hardware read (all zeroes). Ignoring to prevent graph dips.");
    return false;
  }

  data.battery.runtime_minutes = runtime_mins;
  data.battery.level = battery_pct;
  
  // Parse Status
  std::string status_str = packed.substr(24, 8);
  bool on_battery = (status_str.length() > 0 && status_str[0] == '1'); 

  // --- TIME-BASED DEBOUNCE LOGIC ---
  static bool is_full = true;
  static int below_100_count = 0; 
  
  if (battery_pct >= 100.0f) {
      is_full = true;
      below_100_count = 0; // Reset counter immediately on a 100% read
  } else {
      below_100_count++;
      // Require 2 consecutive reads below 100% to drop the 'Full' status
      if (below_100_count >= 2) {
          is_full = false;
      }
  }

  if (on_battery) {
      data.power.status = "OB DISCHRG";
      data.battery.status = "Discharging";
      is_full = false; // Force reset if we lose AC power
      below_100_count = 2; // Keep counter aligned with state
  } else {
      if (is_full) {
          data.power.status = "OL";
          data.battery.status = "Full";
      } else {
          data.power.status = "OL CHRG";
          data.battery.status = "Charging";
      }
  }

  ESP_LOGI(GM_TAG, "True UPS Data: Batt=%.0f%% | Runtime=%.0f min | Status=%s",
           battery_pct, runtime_mins, data.power.status.c_str());

  return true;
}

bool GoldenMateProtocol::read_data(UpsData &data) {
  bool success = false;

  // CRITICAL HARDWARE TRIGGER: 
  // We MUST poll Report 0x01 first to force the firmware to refresh Report 0x0C
  HidReport status_report;
  read_feature_report(REPORT_ID_STATUS, status_report);

  // Now read the freshly updated ASCII data from Report 0x0C
  HidReport megatec_report;
  if (read_feature_report(REPORT_ID_MEGATEC, megatec_report)) {
    if (parse_megatec_string(megatec_report, data)) {
      success = true;
    }
  }

  // Set actual names on every read cycle
  data.device.manufacturer = "GoldenMate";
  data.device.model = "UPS Pro";
  
  std::string str;
  if (parent_->get_string_descriptor(3, str) == ESP_OK && !str.empty()) {
    data.device.serial_number = str;
  }

  return success;
}

}  // namespace ups_hid
}  // namespace esphome