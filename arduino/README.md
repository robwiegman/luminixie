# LumiNixie Arduino

## Uploading

The software uses SPIFFS to store it's configuration,
so make sure that it's available by setting the appropriate flash size in Arduino.
You can find this setting in the Arduino menu under "Tools" -> "Flash Size".
For example: select `4M (1M SPIFFS)`.

## Running

On startup, the software will try to connect to a WiFi network.
When connecting to the network does not succeed within 15 seconds,
an accesspoint (WiFi network) will be started. It's SSID will be
something like `luminixie-XXXX`.
You can connect to that accesspoint from your computer, use password `luminixie`.

Get the current configuration from http://luminixie/configuration using a GET request.
The configuration is a json document.

Change `wifiSsid` and `wifiPassword` and send the json to http://luminixie/configuration using a POST request.
The ESP-8266 (or whatever board you run) will restart and try to connect to the configured WiFi network.

