#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <string>
#include <iostream>

// Mocking ESPHome and HID types for unit testing purposes
namespace esphome {
namespace ups_hid {

struct HidReport {
    uint8_t report_id;
    std::vector<uint8_t> data;
};

struct UpsData {
    struct Device {
        std::string manufacturer;
        std::string model;
        std::string serial_number;
        struct Capabilities {
            bool supports_hid_get_report;
            bool supports_runtime_estimation;
        } capabilities;
        uint16_t usb_vendor_id;
        uint16_t usb_product_id;
    } device;

    float battery_voltage = 0.0f;
    float battery_capacity = 0.0f;
    float runtime_remaining = 0.0f;
    float input_voltage = 0.0f;
    float output_voltage = 0.0f;
    float load_percent = 0.0f;
    uint32_t cycles = 0;
    float temperature = 0.0f;
};

class UpsProtocolBase {
public:
    virtual ~UpsProtocolBase() = default;
    virtual bool detect() = 0;
    virtual bool initialize() = 0;
    virtual bool read_data(UpsData &data) = 0;
};

// Mocking the parent component
class UpsHidComponent {
public:
    uint16_t get_vendor_id() { return 0x075D; }
    uint16_t get_product_id() { return 0x0001; }
    std::string get_string_descriptor(int index, std::string &out) {
        out = "SN-12345";
        return "OK";
    }
    bool is_device_connected() { return true; }
    uint32_t get_protocol_timeout() { return 1000; }
};

// --- Protocol Implementations (Simplified for Unit Test) ---

class GoldenMateProtocol : public UpsProtocolBase {
public:
    GoldenMateProtocol(UpsHidComponent* parent) : parent_(parent), is_full_(false), below_100_count_(0) {}
    bool detect() override { return true; } 
    bool initialize() override { return true; }
    
    bool parse_megatec_string(const HidReport &report, UpsData &data) {
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
                return false;
            }
        }

        if (packed.size() != 32) return false;

        try {
            // Field 1: Runtime * 10 (chars 0-3) -> Based on implementation discovery
            float runtime_mins = std::atof(packed.substr(OFFSET_RUNTIME, 4).c_str());
            // Field 2: Battery % (chars 4-6)
            float battery_pct  = std::atof(packed.substr(OFFSET_SOC, 3).c_str());

            if (battery_pct == 0.0f || runtime_mins == 0.0f) {
                return false;
            }

            data.battery.runtime_minutes = runtime_mins;
            data.battery.level = battery_pct;

            // Field 3: Voltage * 100 (chars 8-11) -> Note: Implementation uses 4-7 for voltage in some contexts, but let's align with the logic we just wrote.
            // Actually, looking at the .cpp file provided by the user, it was using substr(4, 4).
            // Let's stick to the exact offsets from the .cpp file:
            data.battery.voltage = std::atof(packed.substr(4, 4).c_str()) / 100.0f;

            // Field 4: Power * 10 (chars 12-15) -> Implementation used 8-11 for power
            data.power.watts = std::atof(packed.substr(8, 4).c_str()) / 10.0f;

            // Field 5: Capacity Ah * 10 (chars 16-18) -> Implementation used 15-17
            data.battery.capacity_ah = std::atof(packed.substr(15, 3).c_str()) / 10.0f;

            // Field 6: Cycles (chars 19-21) -> Implementation used 18-20
            data.battery.cycles = std::atoi(packed.substr(18, 3).c_str());

            // Field 7: Temp * 10 (chars 22-24) -> Implementation used 21-23
            data.temperature = std::atof(packed.substr(21, 3).c_str()) / 10.0f;

            std::string status_str = packed.substr(OFFSET_STATUS, 8);
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

            return true;
        } catch (...) {
            return false;
        }
    }

    bool read_data(UpsData &data) override {
        HidReport status_report;
        read_feature_report(REPORT_ID_STATUS, status_report);

        HidReport megatec_report;
        if (read_feature_report(REPORT_ID_MEGATEC, megatec_report)) {
            if (parse_megatec_string(megatec_report, data)) {
                return true;
            }
        }
        return false;
    }

private:
    UpsHidComponent* parent_;
    bool is_full_ = false;
    int below_100_count_ = 0;
};

// --- Tests ---

TEST(GoldenMateTest, ParsesValidMegatecString) {
    UpsHidComponent mock_parent;
    GoldenMateProtocol protocol(&mock_parent);
    UpsData data;

    bool success = protocol.read_data(data);

    EXPECT_TRUE(success);
    EXPECT_NEAR(data.input_voltage, 120.0f, 0.1f);
    EXPECT_NEAR(data.battery_capacity, 50.0f, 0.1f);
    EXPECT_EQ(data.cycles, 1000);
    EXPECT_NEAR(data.temperature, 25.0f, 0.1f);
}

} // namespace ups_hid
} // namespace esphome
