import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from .. import CONF_A2DP_SINK_ID, A2DPSink, a2dp_sink_ns

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp_sink"]

A2DPSinkBitrateSensor = a2dp_sink_ns.class_(
    "A2DPSinkBitrateSensor",
    sensor.Sensor,
    cg.Component,
)

A2DPSinkBitDepthSensor = a2dp_sink_ns.class_(
    "A2DPSinkBitDepthSensor",
    sensor.Sensor,
    cg.Component,
)

CONF_BITRATE_SENSOR = "bitrate_sensor"
CONF_BIT_DEPTH_SENSOR = "bit_depth_sensor"

SENSOR_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.GenerateID(CONF_A2DP_SINK_ID): cv.use_id(A2DPSink),
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_BITRATE_SENSOR): SENSOR_SCHEMA,
        cv.Optional(CONF_BIT_DEPTH_SENSOR): SENSOR_SCHEMA,
    }
)


async def to_code(config: ConfigType) -> None:
    if CONF_BITRATE_SENSOR in config:
        var = await sensor.new_sensor(config[CONF_BITRATE_SENSOR])
        await cg.register_component(var, config[CONF_BITRATE_SENSOR])
        await cg.register_parented(var, config[CONF_BITRATE_SENSOR][CONF_A2DP_SINK_ID])

    if CONF_BIT_DEPTH_SENSOR in config:
        var = await sensor.new_sensor(config[CONF_BIT_DEPTH_SENSOR])
        await cg.register_component(var, config[CONF_BIT_DEPTH_SENSOR])
        await cg.register_parented(var, config[CONF_BIT_DEPTH_SENSOR][CONF_A2DP_SINK_ID])
