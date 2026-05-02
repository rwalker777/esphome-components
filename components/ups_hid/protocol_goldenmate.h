#pragma once

#include "ups_hid.h"

namespace esphome {
namespace ups_hid {

/**
 * GoldenMate / BMS Smart-Battery HID Protocol
 *
 * Vendor: 0x06DA (-BMS-), Product: 0xFFFF (Smart-Battery)
 * Used by GoldenMate 1000VA Pro and similar BMS-based UPS units.
 *
 * Data sources:
 *   Report 0x01 (Feature, 21 bytes) - binary status:
 *     byte 11: battery % (0-100)
 *     bytes 12-13 LE16: runtime to empty (seconds)
 *     byte 16: nominal input voltage (static)
 *     byte 20: temperature (°C, BMS internal)
 *
 *   Report 0x0C (Feature, 65 bytes) - contains packed Megatec ASCII at bytes 30-61:
 *     "LLLL FFFF OOOO BBB FFF VVV TTT SSSSSSSS"
 *     Field 1 (4 chars): Load % × 10 (e.g., "0107" = 10.7%)
 *     Field 2 (4 chars): Low voltage transfer (e.g., "1400" = 140.0V)
 *     Field 3 (4 chars): High voltage transfer (e.g., "2084" = 208.4V)
 *     Field 4 (3 chars): Battery % (e.g., "099" = 99%)
 *     Field 5 (3 chars): Frequency × 10 (e.g., "599" = 59.9 Hz)
 *     Field 6 (3 chars): Battery voltage × 100 (e.g., "205" = 2.05V)
 *     Field 7 (3 chars): Temperature × 10 (e.g., "350" = 35.0°C)
 *     Field 8 (8 chars): Status bits ("01000000" = on battery)
 *       bit 6: on battery (1 = AC lost)
 */
class GoldenMateProtocol : public UpsProtocolBase {
 public:
  GoldenMateProtocol(UpsHidComponent *parent) : UpsProtocolBase(parent) {}

  bool detect() override;
  bool initialize() override;
  bool read_data(UpsData &data) override;
  DeviceInfo::DetectedProtocol get_protocol_type() const override { return DeviceInfo::PROTOCOL_GENERIC_HID; }
  std::string get_protocol_name() const override { return "GoldenMate BMS"; }

 private:
  static const uint8_t REPORT_ID_STATUS = 0x01;
  static const uint8_t REPORT_ID_MEGATEC = 0x0C;

  struct HidReport {
    uint8_t report_id;
    std::vector<uint8_t> data;
    HidReport() : report_id(0) {}
  };

  bool read_feature_report(uint8_t report_id, HidReport &report);
  bool parse_binary_status(const HidReport &report, UpsData &data);
  bool parse_megatec_string(const HidReport &report, UpsData &data);
  std::string extract_packed_ascii(const uint8_t *data, size_t offset, size_t len);
};

}  // namespace ups_hid
}  // namespace esphome
