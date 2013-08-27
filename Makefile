obj-y                           += gpio/
obj-$(CONFIG_GPS) 	        += gps/
obj-y 		                += hsi/
obj-$(CONFIG_HWMON)             += hwmon/
obj-y				+= i2c/
obj-$(CONFIG_INPUT)             += input/
obj-y				+= leds/
obj-y				+= misc/
obj-$(CONFIG_MDM_CTRL)		+= modem_control/
obj-y				+= nfc/
obj-y				+= pinctrl/
obj-y                           += platform/x86/
obj-$(CONFIG_POWER_SUPPLY)	+= power/
obj-$(CONFIG_REGULATOR)         += regulator/
obj-$(CONFIG_STAGING)		+= staging/
obj-y               		+= hsu/
obj-y               		+= mfd/
obj-y				+= net/wireless/
obj-$(CONFIG_WATCHDOG)		+= watchdog/
