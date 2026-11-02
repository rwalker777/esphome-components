"""Exercise metadata through the ESPHome schema, before code generation."""

import importlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "components"))
from ups_hid.sensor import CONFIG_SCHEMA
from ups_hid import FILTER_SOURCE_FILES, CONFIG_SCHEMA as HUB_SCHEMA
from esphome.core import CORE
from esphome.loader import ComponentManifest


class SensorMetadataTest(unittest.TestCase):
    def config(self, kind, **overrides):
        CORE.reset()
        return CONFIG_SCHEMA(
            dict(id="metric", name="Metric", ups_hid_id="ups", type=kind, **overrides)
        )

    def test_metadata_for_home_assistant(self):
        for kind, unit, device_class, decimals in [
            ("battery_level", "%", "battery", 0),
            ("input_voltage", "V", "voltage", 1),
            ("output_voltage", "V", "voltage", 1),
            ("output_power", "W", "power", 1),
            ("load_percent", "%", "", 0),
            ("runtime", "min", "duration", 0),
            ("frequency", "Hz", "frequency", 1),
            ("output_frequency", "Hz", "frequency", 1),
            ("output_frequency_nominal", "Hz", "frequency", 0),
            ("output_current", "A", "current", 1),
            ("output_voltage_nominal", "V", "voltage", 0),
            ("ups_apparent_power_nominal", "VA", "apparent_power", 0),
            ("ups_watchdog", "s", "duration", 0),
        ]:
            with self.subTest(kind=kind):
                config = self.config(kind)
                self.assertEqual(config["unit_of_measurement"], unit)
                self.assertEqual(config["device_class"], device_class)
                self.assertEqual(config["accuracy_decimals"], decimals)

    def test_user_metadata_takes_precedence(self):
        config = self.config(
            "battery_level",
            unit_of_measurement="custom",
            device_class="power",
            accuracy_decimals=3,
        )
        self.assertEqual(config["unit_of_measurement"], "custom")
        self.assertEqual(config["device_class"], "power")
        self.assertEqual(config["accuracy_decimals"], 3)

    def test_optional_platform_files(self):
        CORE.reset()
        self.assertEqual(len(FILTER_SOURCE_FILES()), 10)
        # Each platform manifest scans the package too; all must share the filter.
        for name in ["sensor", "binary_sensor", "text_sensor", "button", "number"]:
            module = importlib.import_module("ups_hid." + name)
            files = {
                resource.resource for resource in ComponentManifest(module).resources
            }
            self.assertNotIn("control_number.h", files)
            self.assertNotIn("control_button.cpp", files)
            self.assertNotIn("sensor_numeric.cpp", files)
        CORE.add_define("USE_SENSOR")
        self.assertNotIn("sensor_numeric.cpp", FILTER_SOURCE_FILES())
        self.assertNotIn("sensor_numeric.h", FILTER_SOURCE_FILES())
        self.assertIn("control_number.h", FILTER_SOURCE_FILES())
        for define in [
            "USE_BINARY_SENSOR",
            "USE_TEXT_SENSOR",
            "USE_BUTTON",
            "USE_NUMBER",
        ]:
            CORE.add_define(define)
        self.assertEqual(FILTER_SOURCE_FILES(), [])

    def test_protocol_configuration(self):
        for protocol in ["auto", "generic", "tripplite"]:
            CORE.reset()
            self.assertEqual(
                HUB_SCHEMA({"id": "ups", "protocol": protocol})["protocol"], protocol
            )
        self.assertFalse(HUB_SCHEMA({"id": "ups"})["diagnostic_dump"])
        self.assertTrue(
            HUB_SCHEMA({"id": "ups", "diagnostic_dump": True})["diagnostic_dump"]
        )


if __name__ == "__main__":
    unittest.main()
