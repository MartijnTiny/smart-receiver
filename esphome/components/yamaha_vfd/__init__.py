"""Yamaha RX-V540RDS display-bus sniffer + IR control (Smart Receiver, Phase 3).

Reads the IC601 -> IC501 (M66003 VFD controller) bus with a level-3 GPIO
interrupt on core 1 and sends Yamaha NEC codes via ESPHome's
remote_transmitter on the IR receiver's output line (REM1).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import remote_transmitter
from esphome.const import CONF_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["remote_transmitter"]
CODEOWNERS = []

CONF_CS_PIN = "cs_pin"
CONF_SCK_PIN = "sck_pin"
CONF_DATA_PIN = "data_pin"
CONF_TRANSMITTER_ID = "transmitter_id"
CONF_ON_CHANGE = "on_change"

yamaha_vfd_ns = cg.esphome_ns.namespace("yamaha_vfd")
YamahaVfd = yamaha_vfd_ns.class_("YamahaVfd", cg.Component)
ChangeTrigger = yamaha_vfd_ns.class_("ChangeTrigger", automation.Trigger.template())

# Plain GPIO numbers < 32: the capture loop reads all three pins with one
# GPIO_IN_REG read, so they must all live in the low GPIO bank.
_GPIO_LOW = cv.int_range(min=0, max=31)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(YamahaVfd),
            cv.Required(CONF_CS_PIN): _GPIO_LOW,
            cv.Required(CONF_SCK_PIN): _GPIO_LOW,
            cv.Required(CONF_DATA_PIN): _GPIO_LOW,
            cv.Required(CONF_TRANSMITTER_ID): cv.use_id(
                remote_transmitter.RemoteTransmitterComponent
            ),
            # Fires whenever decoded state or the display text changes; use it
            # to component.update the template entities (push, not poll).
            cv.Optional(CONF_ON_CHANGE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ChangeTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config[CONF_CS_PIN], config[CONF_SCK_PIN], config[CONF_DATA_PIN]))
    tx = await cg.get_variable(config[CONF_TRANSMITTER_ID])
    cg.add(var.set_transmitter(tx))
    for conf in config.get(CONF_ON_CHANGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
