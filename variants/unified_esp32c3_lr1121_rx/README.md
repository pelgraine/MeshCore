# Unified ESP32-C3 LR1121 ELRS receiver (experimental)

Firmware for compatible unified ESP32-C3/LR1121
ExpressLRS receivers.
The pinout, RF switch and tx drive range are taken from the ExpressLRS GEPRC Nano 900MHz V3
firmware.

The repeater, ESP-NOW bridge, BLE companion and Wi-Fi companion also have
`_i2c` prefixed targets (for example, `_i2c_repeater`) for an INA219
at address 0x40.
These use the RX pad (GPIO20) for SDA and TX pad (GPIO21) for SCL, releasing
UART0 at startup and disabling the serial console.

Use the ExpressLRS web updater to install the `firmware.bin`.
After installation, BLE pairing uses PIN 123456. Hold the button (GPIO9) for five seconds
after startup or use `start ota` to enable the MeshCore updater at
http://192.168.4.1/update on the MeshCore-OTA access point. The RGB LED
indicates receive (green), transmit (red) and OTA (blue).

The repeater supports OTA via the `start ota` administrator command. Its
default administrator password is `password`.

Initial repeater flashing via OTA will bring up the repeater on the default
preset, which may be inconvenient seeing as there's no USB console to reconfigure 
the repeater to your local preset.
To avoid this, first OTA flash with BLE or wifi companion firmware then connect and 
configure the radio preset. Then OTA flash with repeater firmware, and the radio
preset previously configured will be picked up.

Due to flash repartitioning restrictions, the companion targets are limited to 120 contacts.
Increasing this number can lead to configuration and contacts not saving.
 
