include $(BOARD_DIR)/firmware/firmware.mk


BOARDINC += $(BOARD_DIR)/generated/controllers/generated

# defines SHORT_BOARD_NAME
include $(BOARD_DIR)/meta-info.env

# this would save some flash while being unable to update WBO controller firmware
# PRO-MS6 has an on-board rusEFI wideband (U1005): keep CAN firmware update enabled
# DDEFS += -DEFI_WIDEBAND_FIRMWARE_UPDATE=FALSE

# assign critical LED to a non-existent pin if you do not have it on your board
# good old PD14 is still the default value
# PD14 is the ETB PWM on PRO-MS6, critical LED is D206 red on PD4
DDEFS += -DLED_CRITICAL_ERROR_BRAIN_PIN=Gpio::D4

# EGT chip
#un-comment to enable
#DDEFS += -DEFI_MAX_31855=TRUE

#see main repo for details on this any many other optional subsystems. We have too many, one has to choose what fits into his choice of stm32
#DDEFS += -DEFI_ONBOARD_MEMS=TRUE
