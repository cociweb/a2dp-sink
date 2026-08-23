from dataclasses import dataclass
import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, ID
from esphome.cpp_generator import TemplateArgsType
import esphome.final_validate as fv
from esphome.types import ConfigType

try:
    from esphome.components.esp32 import idf_version, request_bluetooth
except ImportError:
    # ESPHome < 2026.8 wrote these sdkconfig flags directly from each component.
    def request_bluetooth() -> None:
        add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)

    def idf_version() -> cv.Version:
        return cv.Version(5, 0, 0)

_LOGGER = logging.getLogger(__name__)
DOMAIN = "a2dp"

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["ring_buffer"]

CONF_A2DP_ID = "a2dp_id"
CONF_DEVICE_NAME = "device_name"
CONF_AUTO_START = "auto_start"
CONF_AUTO_RECONNECT = "auto_reconnect"
CONF_RING_BUFFER_SIZE = "ring_buffer_size"
CONF_USE_PSRAM = "use_psram"
CONF_DISCOVERABLE_DURATION = "discoverable_duration"
CONF_KEEP_DISCOVERABLE_AFTER_CONNECT = "keep_discoverable_after_connect"
CONF_PAIRING_PIN = "pairing_pin"
CONF_PREFERRED_SAMPLE_RATE = "preferred_sample_rate"
CONF_PREFERRED_BITS_PER_SAMPLE = "preferred_bits_per_sample"
CONF_COEXISTENCE = "coexistence"
CONF_SOFTWARE_COEXISTENCE = "software_coexistence"
CONF_PREFER_BT_WHILE_STREAMING = "prefer_bt_while_streaming"
CONF_PREFER_BT_WHILE_DISCOVERABLE = "prefer_bt_while_discoverable"
CONF_PAUSE_WIFI_SOURCES_ON_CONNECT = "pause_wifi_sources_on_connect"

BLE_COMPONENTS = {
    "bluetooth_proxy",
    "esp32_ble",
    "esp32_ble_tracker",
    "esp32_ble_client",
    "esp32_ble_server",
    "esp32_ble_beacon",
}


@dataclass
class A2DPData:
    ble_required: bool = False
    has_wifi: bool = False


def _data() -> A2DPData:
    if DOMAIN not in CORE.data:
        CORE.data[DOMAIN] = A2DPData()
    return CORE.data[DOMAIN]


SAMPLE_RATE_BUILD_FLAGS = {
    44100: "A2D_SBC_IE_SAMP_FREQ_44",
    48000: "A2D_SBC_IE_SAMP_FREQ_48",
}


def validate_pairing_pin(value):
    value = cv.string_strict(value)
    if not 1 <= len(value) <= 16:
        raise cv.Invalid("pairing_pin must be between 1 and 16 characters")
    return value

a2dp_ns = cg.esphome_ns.namespace("a2dp")
A2DP = a2dp_ns.class_("A2DP", cg.Component)

A2DPEnableAction = a2dp_ns.class_(
    "A2DPEnableAction",
    automation.Action,
    cg.Parented.template(A2DP),
)
A2DPDisableAction = a2dp_ns.class_(
    "A2DPDisableAction",
    automation.Action,
    cg.Parented.template(A2DP),
)
A2DPRestartDiscoveryAction = a2dp_ns.class_(
    "A2DPRestartDiscoveryAction",
    automation.Action,
    cg.Parented.template(A2DP),
)

COEXISTENCE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SOFTWARE_COEXISTENCE, default=True): cv.boolean,
        cv.Optional(CONF_PREFER_BT_WHILE_STREAMING, default=True): cv.boolean,
        cv.Optional(CONF_PREFER_BT_WHILE_DISCOVERABLE, default=False): cv.boolean,
        cv.Optional(CONF_PAUSE_WIFI_SOURCES_ON_CONNECT, default=False): cv.boolean,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(A2DP),
            cv.Optional(CONF_DEVICE_NAME, default="ESPHome"): cv.string,
            cv.Optional(CONF_AUTO_START, default=False): cv.boolean,
            cv.Optional(CONF_AUTO_RECONNECT, default=False): cv.boolean,
            cv.Optional(CONF_RING_BUFFER_SIZE, default=131072): cv.int_range(
                min=16384, max=4194304
            ),
            cv.Optional(CONF_USE_PSRAM, default=False): cv.boolean,
            cv.Optional(CONF_DISCOVERABLE_DURATION): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_KEEP_DISCOVERABLE_AFTER_CONNECT, default=False): cv.boolean,
            cv.Optional(CONF_PAIRING_PIN): validate_pairing_pin,
            cv.Optional(CONF_PREFERRED_SAMPLE_RATE, default="auto"): cv.Any(
                "auto", cv.one_of(44100, 48000, int=True)
            ),
            cv.Optional(CONF_PREFERRED_BITS_PER_SAMPLE, default=16): cv.one_of(
                16, 32, int=True
            ),
            cv.Optional(CONF_COEXISTENCE): COEXISTENCE_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


def final_validate(config: ConfigType) -> ConfigType:
    data = _data()
    full_config = fv.full_config.get()
    data.ble_required = any(component in full_config for component in BLE_COMPONENTS)
    data.has_wifi = "wifi" in full_config
    if data.ble_required:
        _LOGGER.warning(
            "a2dp uses Bluetooth Classic (A2DP/AVRCP). Combining it with ESPHome BLE "
            "components on original ESP32 is unsupported: the BLE stack enables BLE-only "
            "mode and releases Classic controller memory."
        )
    return config


FINAL_VALIDATE_SCHEMA = final_validate

A2DP_ACTION_SCHEMA = automation.maybe_simple_id(
    cv.Schema({cv.GenerateID(): cv.use_id(A2DP)})
)


@automation.register_action(
    "a2dp.enable",
    A2DPEnableAction,
    A2DP_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp.disable",
    A2DPDisableAction,
    A2DP_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp.restart_discovery",
    A2DPRestartDiscoveryAction,
    A2DP_ACTION_SCHEMA,
    synchronous=True,
)
async def a2dp_action_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: TemplateArgsType,
):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))
    cg.add(var.set_auto_reconnect(config[CONF_AUTO_RECONNECT]))
    cg.add(var.set_ring_buffer_size(config[CONF_RING_BUFFER_SIZE]))
    cg.add(var.set_use_psram(config[CONF_USE_PSRAM]))
    cg.add(var.set_preferred_bits_per_sample(config[CONF_PREFERRED_BITS_PER_SAMPLE]))
    if config[CONF_PREFERRED_SAMPLE_RATE] != "auto":
        cg.add_build_flag(
            f"-DBTC_AV_SBC_DEFAULT_SAMP_FREQ={SAMPLE_RATE_BUILD_FLAGS[config[CONF_PREFERRED_SAMPLE_RATE]]}"
        )
    if CONF_DISCOVERABLE_DURATION in config:
        cg.add(var.set_discoverable_duration_ms(config[CONF_DISCOVERABLE_DURATION].total_milliseconds))
    cg.add(var.set_keep_discoverable_after_connect(config[CONF_KEEP_DISCOVERABLE_AFTER_CONNECT]))
    if CONF_PAIRING_PIN in config:
        cg.add(var.set_pairing_pin(config[CONF_PAIRING_PIN]))

    data = _data()
    # Request the BT controller through the 2026.8 network reconciler. This does
    # not load the ESPHome BLE stack or the IDF BLE host APIs; Classic-only
    # firmware keeps CONFIG_BT_BLE_ENABLED off below.
    request_bluetooth()
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)

    if coex := config.get(CONF_COEXISTENCE):
        software_coexistence = coex[CONF_SOFTWARE_COEXISTENCE]
        cg.add(var.set_software_coexistence(software_coexistence))
        cg.add(var.set_prefer_bt_while_streaming(coex[CONF_PREFER_BT_WHILE_STREAMING]))
        cg.add(var.set_prefer_bt_while_discoverable(coex[CONF_PREFER_BT_WHILE_DISCOVERABLE]))
        cg.add(var.set_pause_wifi_sources_on_connect(coex[CONF_PAUSE_WIFI_SOURCES_ON_CONNECT]))
        # Always define this when the coexistence block is present so the C++
        # setters are compiled. The IDF coexist library is only requested when
        # software_coexistence is actually enabled.
        cg.add_define("USE_SOFTWARE_COEXISTENCE")
        if software_coexistence and data.has_wifi:
            # Write the current IDF symbol here. request_software_coexistence()
            # only sets an ESPHome flag; ESPHome 2026.x then emits the pre-5.1
            # CONFIG_SW_COEXIST_ENABLE name, which kconfgen rejects on IDF 5.5+.
            if idf_version() >= cv.Version(5, 1, 0):
                add_idf_sdkconfig_option("CONFIG_ESP_COEX_SW_COEXIST_ENABLE", True)
            else:
                add_idf_sdkconfig_option("CONFIG_SW_COEXIST_ENABLE", True)
        if coex[CONF_PAUSE_WIFI_SOURCES_ON_CONNECT] and data.has_wifi:
            from esphome.components import wifi

            wifi.enable_runtime_power_save_control()
            wifi.enable_runtime_roaming_suppression()
            cg.add_define("USE_A2DP_WIFI_PAUSE")

    add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_A2DP_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_AVRC_TG_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_AVRC_CT_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", data.ble_required)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", not data.ble_required)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", data.ble_required)
    if not data.ble_required:
        # request_bluetooth() would otherwise let the reconciler enable BLE 4.2
        # features. Keep Classic-only A2DP off the BLE host APIs.
        add_idf_sdkconfig_option("CONFIG_BT_BLE_42_FEATURES_SUPPORTED", False)
        add_idf_sdkconfig_option("CONFIG_BT_BLE_50_FEATURES_SUPPORTED", False)
    else:
        cg.add_define("USE_A2DP_BTDM")
    if CONF_PAIRING_PIN in config:
        add_idf_sdkconfig_option("CONFIG_BT_SSP_ENABLED", False)
        add_idf_sdkconfig_option("CONFIG_BT_LEGACY_PIN_PAIRING_ENABLED", True)

    cg.add_define("USE_A2DP")
