#include "ups_hid.h"
#include "tripplite_3024.h"

namespace esphome {
namespace ups_hid {

static const char *const TL_TAG = "ups_hid.tripplite";

class TrippLiteProtocol : public UpsProtocolBase {
 public:
  explicit TrippLiteProtocol(UpsHidComponent *parent) : UpsProtocolBase(parent) {}

  DeviceInfo::DetectedProtocol get_protocol_type() const override { return DeviceInfo::PROTOCOL_TRIPPLITE_HID; }
  std::string get_protocol_name() const override { return "Tripp Lite HID"; }

  bool detect() override {
    return parent_->is_connected() && Tripplite3024::matches(parent_->get_vendor_id(), parent_->get_product_id());
  }

  bool initialize() override {
    if (!detect()) {
      return false;
    }
    uint32_t connection = parent_->get_usb_connection_id();
    std::vector<uint8_t> bytes;
    esp_err_t ret = parent_->get_hid_report_descriptor(bytes, parent_->get_protocol_timeout());
    if (ret != ESP_OK) {
      ESP_LOGE(TL_TAG, "HID descriptor unavailable: %s; refusing report-ID guesses", esp_err_to_name(ret));
      return false;
    }
    // UART logger buffers cannot hold an entire descriptor on one line.
    for (size_t offset = 0; offset < bytes.size(); offset += 16) {
      ESP_LOGVV(TL_TAG, "HID descriptor [%04zu]: %s", offset,
                format_hex_pretty(bytes.data() + offset, std::min(size_t(16), bytes.size() - offset)).c_str());
    }
    if (!decoder_.descriptor.parse(bytes.data(), bytes.size())) {
      ESP_LOGE(TL_TAG, "Unsupported HID descriptor");
      return false;
    }
    if (!detect() || parent_->get_usb_connection_id() != connection) {
      return false;
    }
    connection_id_ = connection;
    ESP_LOGI(TL_TAG, "09AE:3024 descriptor: %zu bytes, %zu fields", bytes.size(), decoder_.descriptor.fields.size());
    for (const auto &f : decoder_.descriptor.fields) {
      ESP_LOGD(TL_TAG, "HID type=%u ID=0x%02X usage=0x%08lX bit=%u size=%u unit=0x%08lX exponent=%d flags=0x%lX",
               unsigned(f.type), f.id, static_cast<unsigned long>(f.usage), f.offset, f.size,
               static_cast<unsigned long>(f.unit), f.exponent, static_cast<unsigned long>(f.flags));
    }
    return true;
  }

  bool read_data(UpsData &data) override {
    uint32_t started = millis(), budget = parent_->get_protocol_timeout();
    bool logged_failure = false;
    bool success = decoder_.read(
        [this, started, budget, &logged_failure](uint8_t type, uint8_t id, uint8_t *buffer, size_t *length) {
          uint32_t elapsed = millis() - started;
          if (elapsed >= budget) {
            return false;
          }
          esp_err_t ret = parent_->hid_get_report(type, id, buffer, length, budget - elapsed);
          if (ret != ESP_OK && !logged_failure) {
            logged_failure = true;
            ESP_LOGW(TL_TAG, "First failed GET_REPORT this poll: type=%u ID=0x%02X: %s", type, id,
                     esp_err_to_name(ret));
          }
          return ret == ESP_OK;
        },
        data);
    if (success && parent_->take_diagnostic_dump(connection_id_)) {
      uint32_t diagnostic_started = millis();
      // Bound this optional, once-per-connection exploration to two seconds.
      decoder_.vendor_diagnostics(
          [this, diagnostic_started, started, budget](uint8_t type, uint8_t id, uint8_t *buffer, size_t *length) {
            uint32_t elapsed = millis() - diagnostic_started;
            uint32_t total_elapsed = millis() - started;
            if (elapsed >= 2000 || total_elapsed >= budget || parent_->get_usb_connection_id() != connection_id_) {
              return false;
            }
            uint32_t timeout = std::min({uint32_t(500), 2000 - elapsed, budget - total_elapsed});
            return parent_->hid_get_report(type, id, buffer, length, timeout) == ESP_OK;
          },
          [](uint8_t id, uint32_t usage, const std::vector<uint8_t> &bytes) {
            ESP_LOGI(TL_TAG, "Vendor diagnostic ID=0x%02X usage=0x%08lX raw=%s", id, static_cast<unsigned long>(usage),
                     bytes.empty() ? "unavailable" : format_hex_pretty(bytes.data(), bytes.size()).c_str());
          });
    }
    return success;
  }

  bool beeper_enable() override { return command_(Tripplite3024::Command::BEEPER_ENABLE); }
  bool beeper_disable() override { return command_(Tripplite3024::Command::BEEPER_DISABLE); }
  bool beeper_mute() override { return command_(Tripplite3024::Command::BEEPER_MUTE); }
  bool start_battery_test_quick() override { return command_(Tripplite3024::Command::TEST_QUICK); }
  bool stop_battery_test() override { return command_(Tripplite3024::Command::TEST_CANCEL); }

  // Deep tests, beeper tests and power/timer writes remain unsupported.
 private:
  Tripplite3024 decoder_;
  uint32_t connection_id_{0};

  bool command_(Tripplite3024::Command command) {
    if (!detect() || parent_->get_usb_connection_id() != connection_id_) {
      return false;
    }
    uint32_t started = millis(), budget = parent_->get_protocol_timeout();
    auto remaining = [&]() {
      uint32_t elapsed = millis() - started;
      return elapsed < budget ? budget - elapsed : 0;
    };
    bool success = decoder_.command(
        command,
        [&](uint8_t type, uint8_t id, uint8_t *bytes, size_t *length) {
          uint32_t timeout = remaining();
          return timeout && parent_->get_usb_connection_id() == connection_id_ &&
                 parent_->hid_get_report(type, id, bytes, length, timeout) == ESP_OK;
        },
        [&](uint8_t type, uint8_t id, const uint8_t *bytes, size_t length) {
          uint32_t timeout = remaining();
          return timeout && parent_->hid_set_report(type, id, bytes, length, timeout, connection_id_) == ESP_OK;
        });
    if (success) {
      ESP_LOGI(TL_TAG, "Control report sent; refreshing actual device status");
      // Never publish a requested command as though it were a measured state.
      parent_->request_control_refresh();
    }
    return success;
  }
};

std::unique_ptr<UpsProtocolBase> create_tripplite_protocol(UpsHidComponent *parent) {
  return std::make_unique<TrippLiteProtocol>(parent);
}

}  // namespace ups_hid
}  // namespace esphome
