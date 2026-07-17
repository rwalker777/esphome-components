#pragma once

#include "ups_hid.h"

namespace esphome {
namespace ups_hid {

/**
 * GoldenMate UPS Protocol
 *
 * Vendor: 0x075D, Product: (Various)
 * Used by GoldenMate UPS units over USB.
 *
 * Data sources:
 * Report 0x01 (Feature, 21 bytes) - binary status:
 * byte 11: battery % (0-100)
 * bytes 12-13 LE16: runtime to empty (seconds)
 *
 * Report 0x0C (Feature, 64+ bytes) - contains packed ASCII at bytes 30-61:
 * Field 1 (4 chars): Current × 100 (e.g., "1491" = 14.91 A) -> mapped to load_percent
 * Field 2 (4 chars): Voltage × 100 (e.g., "1400" = 14.00 V) -> mapped to battery.voltage
 * Field 3 (4 chars): Power × 10 (e.g., "2084" = 208.4 W) -> mapped to input_voltage
 * Field 4 (3 chars): SOC % (e.g., "100" = 100 %) -> mapped to battery.level
 * Field 5 (3 chars): Capacity Ah × 10 (e.g., "599" = 59.9 Ah) -> mapped to frequency
 * Field 6 (3 chars): Cycles (e.g., "205" = 205) -> mapped to output_voltage
 * Field 7 (3 chars): Temp × 10 (e.g., "350" = 35.0 °C) -> mapped to input_transfer_low
 * Field 8 (8 chars): Status bits
 */
class GoldenMateProtocol : public UpsProtocolBase {
 public:
  GoldenMateProtocol(UpsHidComponent *parent) : UpsProtocolBase(parent) {}

  bool detect() override;
  bool initialize() override;
  bool read_data(UpsData &data) override;
  DeviceInfo::DetectedProtocol get_protocol_type() const override { return DeviceInfo::PROTOCOL_GENERIC_HID; }
  std::string get_protocol_name() const override { return "GoldenMate"; }

 private:
  static const uint8_t REPORT_ID_STATUS = 0x01;
  static const uint8_t REPORT_ID_MEGATEC = 0x0C;

  bool is_full_ = true;
  int below_100_count_ = 0;

  struct HidReport {
    uint8_t report_id;
    std::vector<uint8_t> data;
    HidReport() : report_id(0) {}
  };

  bool read_feature_report(uint8_t report_id, HidReport &report);
  bool parse_binary_status(const HidReport &report, UpsData &data);
  bool parse_megatec_string(const HidReport &report, UpsData &data);
};

}  // namespace ups_hid
}  // namespace esphome
