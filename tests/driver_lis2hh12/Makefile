include ../Makefile.tests_common

# as default we use the I2C mode for now, as the SPI mode is not supported, yet
DRIVER ?= lis2hh12_i2c

USEMODULE += xtimer
USEMODULE += $(DRIVER)

FEATURES_REQUIRED += periph_cpuid
FEATURES_REQUIRED += periph_status
FEATURES_REQUIRED += periph_status_extended

include $(RIOTBASE)/Makefile.include
