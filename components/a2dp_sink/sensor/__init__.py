import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_NAME, CONF_TYPE
from esphome.types import ConfigType

from .. import CONF_A2DP_SINK_ID, A2DPSink, a2dp_sink_ns

CODEOWNERS = ["@cociweb"]
DEPENDENCIES = ["a2dp_sink"]

SENSOR_TYPES = {
    "bitrate": "A2DP_SINK_SENSOR_BITRATE",
    "bit_depth": "A2DP_SINK_SENSOR_BIT_DEPTH",
}

A2DPSinkSensor = a2dp_sink_ns.class_(
    "A2DPSinkSensor",
    sensor.Sensor,
    cg.Component,
)

CONFIG_SCHEMA = sensor.sensor_schema(A2DPSinkSensor).extend(
    {
        cv.GenerateID(CONF_A2DP_SINK_ID): cv.use_id(A2DPSink),
        cv.Required(CONF_TYPE): cv.one_of(*SENSOR_TYPES, lower=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config: ConfigType) -> None:
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_A2DP_SINK_ID])
    cg.add(var.set_sensor_type(cg.RawExpression(SENSOR_TYPES[config[CONF_TYPE]])))
    cg.add_define("USE_A2DP_SINK_SENSOR")
