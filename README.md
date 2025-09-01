# ESP32 WIFI to USB-Modbus Bridge

Hello, 
this Project is mainly for connecting a Meltem Gateway (https://www.meltem.com/) via an ESP32 to HomeAssistant (HA).

The Meltem gateway exposes a Modbus COM RTU interface via the Micro-USB interface. So you can either directly connect the Meltem Gateway via USB to your physical HA Instance (like here: https://community.home-assistant.io/t/meltem-wrg-ii-integration-via-meltem-gateway-and-modbus/) or you use this ESP32 Bridge (if you are running HA as VM, or if your HA-physical Server is far away from the Meltem Gateway.

I used a diymore ESP32-S3 DevKitC-1 N16R8 Modul with 2 USB-C Connectors (one for Serial, one OTG).
I had to short (bridge) the OTG and the 5V IN/Out solderings Pads on the ESP32 Board.
