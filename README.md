# ESP32 WIFI to USB-Modbus Bridge

Hello, 
this Project is mainly for connecting a Meltem Gateway (https://www.meltem.com/) via an ESP32-S3 DevKitC-1 (with USB-C OTG) to HomeAssistant (HA).

The Meltem gateway exposes a Modbus COM RTU interface via the Micro-USB interface. So you can either directly connect the Meltem Gateway via USB to your physical HA Instance (like here: https://community.home-assistant.io/t/meltem-wrg-ii-integration-via-meltem-gateway-and-modbus/) or you use this ESP32 Bridge (if you are running HA as VM, or if your HA-physical Server is far away from the Meltem Gateway.

I used a diymore ESP32-S3 DevKitC-1 N16R8 Modul with 2 USB-C Connectors (one for Serial, one OTG).
I had to short (bridge) the OTG and the 5V IN/Out solderings Pads on the ESP32 Board.


So this is the way i got this working: 
1.) Download and install the Espress IF IDF - i used this (https://dl.espressif.com/dl/idf-installer/esp-idf-tools-setup-online-2.3.5.exe)
2.) Download all the files from here an keep the folder structure.

ESP/  <-- root folder
  ├─ sdkconfig.defaults
  ├─ CMakeLists.txt
  └─ main/
     ├─ main.c
     ├─ app_config_example.h
     ├─ idf_component.yml
     └─ CMakeLists.txt

3.) Open "app_config_example.h", edit as needed and save a copy as "app_config.h"
4.) Open a Terminal, navigate to your projectfolder root and do "idf.py set-target esp32s3" (if you have an ESP32 S3), after that run "idf.py build"
5.) Connect your ESP32 device to your Computer (via USB-C "COM", not "OTG"). The ESP should be recordnized as (for example) COM3. Run "idf.py -p COM3 flash monitor"
6.) After flashing the ESP32 you can see Logging on the Terminal. At this point it should at least connect to your wifi.
You can now plugin your Meltem Bridge (or maybe other Modbus COM RTU devices) via USV to the OTG USB-C Connector on the ESP32. 

Now you have: USB-C Power to ESP32 "COM" -> ESP32 OTG to Meltem Bridge, and the ESP32 connected to your WiFi.
In HomeAssistant, you can now add a new modbus device via configuration.yaml

modbus:
  - name: meltem
    type: rtuovertcp
    host: 192.168.x.x   <- IP Address ESP32
    port: 5020
    delay: 1
    timeout: 2

and add the sensors as needed (https://community.home-assistant.io/t/meltem-wrg-ii-integration-via-meltem-gateway-and-modbus/720906 - Great Work and thanky to user "da_anton")

#####################################
#  CONFIG FEATURES IN app_config.h  #
######################################

Create main/app_config.h by copying main/app_config_example.h and editing the values you need.
If an option is omitted, a sensible default is used.

Wi-Fi & Networking

#define APP_WIFI_SSID "..."
Your Wi-Fi network name (2.4 GHz).

#define APP_WIFI_PASS "..."
Wi-Fi password.

#define APP_HOSTNAME "meltem-bridge"
Hostname announced to DHCP and used for mDNS. Helpful so you can reach the device by name instead of IP.

#define APP_NET_USE_DHCP 1
1 = get IP via DHCP (recommended).
0 = use static addressing (set the values below).

Static IP block (only used when APP_NET_USE_DHCP == 0)
#define APP_NET_STATIC_IP_ADDR 192,168,178,230
#define APP_NET_STATIC_NETMASK 255,255,255,0
#define APP_NET_STATIC_GATEWAY 192,168,178,1
#define APP_NET_STATIC_DNS1 192,168,178,1
#define APP_NET_STATIC_DNS2 8,8,8,8

Boot sequence & reliability

#define APP_BOOT_IP_WAIT_S 15
How many seconds to wait for a valid IP after Wi-Fi starts.
If no IP is obtained within this window, the device reboots to recover.

#define APP_NET_WATCHDOG_S 60
Network watchdog: if the device stays without an IP for this many seconds (after boot), it will reboot.
Set to 0 to disable the watchdog (not recommended).

mDNS (optional but handy)

#define APP_MDNS_ENABLE 1
1 = enable mDNS so the device is reachable as APP_HOSTNAME.local (e.g., meltem-bridge.local).
0 = disable mDNS.

#define APP_MDNS_INSTANCE "Meltem RTU Bridge"
Human-readable instance label shown by mDNS browsers.

#define APP_MDNS_SVC_TYPE "_modbus"
mDNS service type (kept as _modbus).

#define APP_MDNS_SVC_PROTO "_tcp"
Protocol for the service (keep _tcp). The service is advertised on port 5020.

Tip: If you use a home router that already resolves DHCP hostnames (e.g., many Fritz!Box models), you may reach the device as meltem-bridge.fritz.box. mDNS adds meltem-bridge.local for zero-config name resolution on most systems.

RGB Status LED

#define APP_LED_KILL_ENABLE 1
Enable/disable the kill pin that can hard-disable the RGB LED. 

#define APP_LED_KILL_PIN 46
GPIO used as a pull-up input. If this pin is tied to GND, the RGB LED output is muted.
Use a non-strapping input pin (GPIO 10/11/12/13 are also good candidates on ESP32-S3).

#define APP_LED_BRIGHTNESS 60
LED brightness 0..255 (linear). 0 = off, 255 = full brightness.
This scales all status colors and blinks.

Note: The small red power LED found on many dev boards is hard-wired to 3V3 and cannot be turned off in software.

#######################################
2) Status RGB LED — meaning of colors

The on-board WS2812 RGB LED gives quick visual feedback:

Blue (steady) – Waiting for network
The device is booting and waiting for a valid IP (up to APP_BOOT_IP_WAIT_S seconds).

Yellow (steady) – Idle / Ready
Wi-Fi is up, TCP server is listening on port 5020. No client is currently sending requests.

Green (short blink) – Request OK
A Modbus RTU-over-TCP request has been successfully forwarded to the Meltem device and a valid response was sent back.

Red (short blink) – Error
Something failed during the last transaction (e.g., timeout, bad CRC in response, USB hiccup).
The bridge keeps running and will process further requests.

If the LED stays yellow forever and the device isn’t reachable: it likely didn’t obtain an IP.
Check SSID/password, power supply, increase APP_BOOT_IP_WAIT_S, or temporarily disable Wi-Fi power-save in code for debugging. The network watchdog (APP_NET_WATCHDOG_S) will auto-reboot if the device loses IP for too long.

#####################################
Quick tips

Home Assistant should use type: rtuovertcp, host: <hostname or IP>, port: 5020.
The bridge accepts RTU frames with or without CRC; if missing, the bridge appends it before sending over USB.

Power & USB OTG: the ESP32-S3 powers the Meltem gateway over USB-OTG. Use a solid 5 V / ≥2 A PSU and a short cable to avoid brown-outs during enumeration
