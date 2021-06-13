# Airsensor using ESP32-BME680
Wifi Airsensor project based on the Bosch BME680 sensor with BSEC library
Designed for low-power usage with Wifi-sleep in between to prevent temperature radiation to BME680
Takes a measurement and then shuts off Wifi for some time
Compiled 05-21. ESP32 1.0.6, Arduino 1.8.14, BSEC Lib 1.6.1480 

# Static IAQ vs IAQ
The main difference between IAQ and static IAQ (sIAQ) relies in the scaling factor calculated based on the recent sensor history. The sIAQ output has been optimized for stationary applications (e.g. fixed indoor devices) whereas the IAQ output is ideal for mobile application (e.g. carry-on devices).