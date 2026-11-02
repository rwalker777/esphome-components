import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_TYPE,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_DEVICE_CLASS,
    CONF_ACCURACY_DECIMALS,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_EMPTY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_APPARENT_POWER,
    UNIT_PERCENT,
    UNIT_VOLT,
    UNIT_MINUTE,
    UNIT_HERTZ,
    UNIT_WATT,
    UNIT_SECOND,
    UNIT_AMPERE,
    UNIT_VOLT_AMPS,
)


from . import FILTER_SOURCE_FILES, ups_hid_ns, UpsHidComponent, CONF_UPS_HID_ID

DEPENDENCIES = ["ups_hid"]

UpsHidSensor = ups_hid_ns.class_("UpsHidSensor", sensor.Sensor, cg.Component)

SENSOR_TYPES = {
    "battery_level": {
        "unit": UNIT_PERCENT,
        "device_class": DEVICE_CLASS_BATTERY,
        "accuracy_decimals": 0,
    },
    "input_voltage": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 1,
    },
    "output_voltage": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 1,
    },
    "output_power": {
        "unit": UNIT_WATT,
        "device_class": DEVICE_CLASS_POWER,
        "accuracy_decimals": 1,
    },
    "load_percent": {
        "unit": UNIT_PERCENT,
        "device_class": DEVICE_CLASS_EMPTY,
        "accuracy_decimals": 0,
    },
    "runtime": {
        "unit": UNIT_MINUTE,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    "frequency": {
        "unit": UNIT_HERTZ,
        "device_class": DEVICE_CLASS_FREQUENCY,
        "accuracy_decimals": 1,
    },
    "output_frequency": {
        "unit": UNIT_HERTZ,
        "device_class": DEVICE_CLASS_FREQUENCY,
        "accuracy_decimals": 1,
    },
    "output_frequency_nominal": {
        "unit": UNIT_HERTZ,
        "device_class": DEVICE_CLASS_FREQUENCY,
        "accuracy_decimals": 0,
    },
    "output_current": {
        "unit": UNIT_AMPERE,
        "device_class": DEVICE_CLASS_CURRENT,
        "accuracy_decimals": 1,
    },
    "output_voltage_nominal": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 0,
    },
    "ups_apparent_power_nominal": {
        "unit": UNIT_VOLT_AMPS,
        "device_class": DEVICE_CLASS_APPARENT_POWER,
        "accuracy_decimals": 0,
    },
    "ups_watchdog": {
        "unit": UNIT_SECOND,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    "battery_voltage": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 1,
    },
    "battery_voltage_nominal": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 0,
    },
    "input_voltage_nominal": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 0,
    },
    "input_transfer_low": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 0,
    },
    "input_transfer_high": {
        "unit": UNIT_VOLT,
        "device_class": DEVICE_CLASS_VOLTAGE,
        "accuracy_decimals": 0,
    },
    "ups_realpower_nominal": {
        "unit": UNIT_WATT,
        "device_class": DEVICE_CLASS_POWER,
        "accuracy_decimals": 0,
    },
    "ups_delay_shutdown": {
        "unit": UNIT_SECOND,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    "ups_delay_start": {
        "unit": UNIT_SECOND,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    "ups_delay_reboot": {
        "unit": UNIT_SECOND,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    # Additional missing sensor types from NUT analysis
    "battery_charge_low": {
        "unit": UNIT_PERCENT,
        "device_class": DEVICE_CLASS_BATTERY,
        "accuracy_decimals": 0,
    },
    "battery_charge_warning": {
        "unit": UNIT_PERCENT,
        "device_class": DEVICE_CLASS_BATTERY,
        "accuracy_decimals": 0,
    },
    "battery_runtime_low": {
        "unit": UNIT_MINUTE,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    "ups_timer_reboot": {
        "unit": UNIT_SECOND,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    "ups_timer_shutdown": {
        "unit": UNIT_SECOND,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
    "ups_timer_start": {
        "unit": UNIT_SECOND,
        "device_class": DEVICE_CLASS_DURATION,
        "accuracy_decimals": 0,
    },
}


def sensor_defaults(config):
    """Apply per-type metadata before ESPHome generates its entity tables."""
    defaults = SENSOR_TYPES[config[CONF_TYPE]]
    for key, source in (
        (CONF_UNIT_OF_MEASUREMENT, "unit"),
        (CONF_DEVICE_CLASS, "device_class"),
        (CONF_ACCURACY_DECIMALS, "accuracy_decimals"),
    ):
        if key not in config and source in defaults:
            config[key] = defaults[source]
    return config


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(UpsHidSensor).extend(
        {
            cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsHidComponent),
            cv.Required(CONF_TYPE): cv.one_of(*SENSOR_TYPES, lower=True),
        }
    ),
    sensor_defaults,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_UPS_HID_ID])
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    sensor_type = config[CONF_TYPE]
    cg.add(var.set_sensor_type(sensor_type))
    cg.add(parent.register_sensor(var, sensor_type))
