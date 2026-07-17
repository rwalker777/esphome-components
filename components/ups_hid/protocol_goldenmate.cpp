#include "protocol_goldenmate.h"
#include "protocol_factory.h"
#include "esphome/core/log.h"
#include <cstring>
#include <cstdlib>

namespace esphome {
namespace ups_hid {

static const char *const GM_TAG = "ups_hid.goldenmate";

// Explicit global creator function to satisfy the factory's extern requirement
std::unique_ptr<UpsProtocolBase> create_goldenmate_protocol(UpsHidComponent* parent) {
  return std::make_unique<GoldenMateProtocol>(parent);
}

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

  data.device.manufacturer = "GoldenMate";
  data.device.model = "UPS Pro";

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

bool GoldenMateProtocol::parse_binary_status(const HidReport &/*report*/, UpsData &/*data*/) {
  return true;
}

bool GoldenMateProtocol::parse_megatec_string(const HidReport &report, UpsData &data) {
  if (report.data.size() < 62) return false;

  const uint8_t *d = report.data.data();
  std::string packed;

  for (int i = 30; i < 62; i++) {
    char c = static_cast<char>(d[i]);
    if (c >= '0' && c <= '9') {
      packed += c;
    } else if (c == ' ' || c == 0x00) {
      packed += '0';
    } else {
      ESP_LOGW(GM_TAG, "Garbage byte detected (0x%02X). Hardware buffer is mid-write, dropping read.", d[i]);
      return false;
    }
  }

  if (packed.size() != 32) return false;

  float runtime_mins = std::atof(packed.substr(0, 4).c_str());
  float battery_pct  = std::atof(packed.substr(12, 3).c_str());

  if (battery_pct == 0.0f || runtime_mins == 0.0f) {
    ESP_LOGW(GM_TAG, "Transient hardware read (0 values detected). Ignoring to prevent graph dips.");
    return false;
  }

  data.battery.runtime_minutes = runtime_mins;
  data.battery.level = battery_pct;

  std::string status_str = packed.substr(24, 8);
  bool on_battery = (status_str.length() > 0 && status_str[0] == '1');

  if (battery_pct >= 100.0f) {
      this->is_full_ = true;
      this->below_100_count_ = 0;
  } else {
      this->below_100_count_++;
      if (this->below_100_count_ >= 2) {
          this->is_full_ = false;
      }
  }

  if (on_battery) {
      data.power.status = "OB DISCHRG";
      data.battery.status = "Discharging";
      this->is_full_ = false;
      this->below_100_count_ = 2;
  } else {
      if (this->is_full_) {
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
  HidReport status_report;
  
  read_feature_report(REPORT_ID_STATUS, status_report);

  HidReport megatec_report;
  if (read_feature_report(REPORT_ID_MEGATEC, megatec_report)) {
    if (parse_megatec_string(megatec_report, data)) {
      success = true;
    }
  }

  return success;
}

}  // namespace ups_hid
}  // namespace esphome
