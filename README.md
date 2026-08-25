# chrWiFi

`chrWiFi` is a small Arduino library namespace that wraps `tzapu/WiFiManager` to provide a practical WiFi lifecycle for ESP devices.

It is designed for projects that need:

- Easy STA startup with saved credentials
- Automatic fallback to AP mode when credentials are missing or repeated STA attempts fail
- Event-driven status reporting
- Optional web portal usage while connected in STA mode
- OTA preparation and failure event notifications

The library currently targets the Arduino framework and is tested in this repository with ESP32 (`esp32dev`).

## Features

- Non-blocking WiFi manager loop (`chrWiFi::loop()`)
- AP name auto-generated from a base name + MAC suffix
- Config portal and web portal controls
- Reconnect interval management
- WiFi signal strength status model
- Stable/unstable connectivity events
- Access to underlying WiFiManager web server for custom endpoints

## Dependency

This library depends on:

- `tzapu/WiFiManager` `^2.0.17`

Defined in [library.json](library.json) and [platformio.ini](platformio.ini).

## Quick Start

Example initialization flow:

```cpp
#include "chrWiFi.h"

void onWiFiEvent(int8_t code, const char* msg) {
	Serial.printf("event=%d msg=%s\n", code, msg ? msg : "null");
}

void setup() {
	Serial.begin(115200);

	chrWiFi::setup("chrWiFiDemo", "12345678", 5000, 15000, 80);
	chrWiFi::onEvent(onWiFiEvent);

	// Try saved STA credentials first; fallback to AP if unavailable.
	chrWiFi::startSta();
}

void loop() {
	chrWiFi::loop();
}
```

For a full working example with custom HTTP handlers, see [examples/Basic/Basic.cpp](examples/Basic/Basic.cpp).

## API Reference (chrWiFi Namespace)

Public methods declared in [src/chrWiFi.h](src/chrWiFi.h):

- `void setup(const char* apName = nullptr, const char* pass = nullptr, uint32_t statusCheckMs = 5387, uint32_t reconnectMs = 30000, uint16_t portalPort = 80)`
	- Initializes internal state, sets AP credentials, timing, and web portal port.
- `void startAP()`
	- Starts config portal in AP mode.
- `void startSta()`
	- Starts STA connection using saved WiFiManager credentials.
- `void stop()`
	- Stops portal/server activity, disconnects WiFi, and switches radio off.
- `void startWebPortal()`
	- Starts WiFiManager web portal while in STA connected mode.
- `void stopWebPortal()`
	- Stops the web portal if active.
- `Status checkStatus()`
	- Returns status from current WiFi mode/connection/signal.
- `Status currentStatus()`
	- Returns last cached status.
- `Status loop()`
	- Must be called repeatedly; processes WiFiManager and reconnect/status logic.
- `bool otaUpdateStarted()`
	- Indicates whether OTA update pre-phase has started.
- `uint32_t ipToUint(const IPAddress &ip)` / `IPAddress uintToIP(uint32_t v)`
	- Utility converters.
- `IPAddress getIp()`
	- Returns STA local IP or AP IP depending on mode.
- `char* getApName()`
	- Returns generated AP name buffer.
- `void onEvent(EventCallback cb)`
	- Registers a global event callback.
- `void setCustomMenuHTML(const char* html)`
	- Injects custom HTML into WiFiManager menu.
- `void setWebServerCallback(std::function<void()> cb)`
	- Callback for web server create/reset events.
- `WiFiManager::WM_WebServer* webServer()`
	- Returns pointer to internal web server for route registration.

## Status Values

`chrWiFi::Status` values:

- `WIFI_OFF_STATUS = -1`
- `WIFI_AP_MODE = 0`
- `WIFI_LOST = 1`
- `WIFI_WEAK = 2`
- `WIFI_MEDIUM = 3`
- `WIFI_STRONG = 4`

Signal classification thresholds (RSSI):

- `< -80 dBm` -> weak
- `< -60 dBm` -> medium
- otherwise strong

## Event Codes

`chrWiFi::EventCode` values:

- `EVENT_ERR = -10`
- `EVENT_OTA_FAILED = -9`
- `EVENT_UNSTABLE = -2`
- `EVENT_WARN = -1`
- `EVENT_OK = 0`
- `EVENT_STABLE = 2`
- `EVENT_NOTICE = 10`
- `EVENT_OTA_PREPARE = 11`
- `EVENT_STATUS = 12`

Typical `EVENT_STATUS` messages include:

- `signal STRONG`
- `signal MEDIUM`
- `signal WEAK`
- `signal LOST`
- `mode: AP`
- `mode: OFF`

## Build

This repository is configured with PlatformIO.

From project root:

```bash
platformio run
```

The default project configuration currently points `src_dir` to `examples/Basic`, so the example sketch is what gets compiled by default.

## Notes

- The default AP password in code is `12345678` if no password is provided.
- AP name is auto-generated as `{base}{MAC[4]}{MAC[5]}` (for example: `ESP-AP-1A2B`).
- Repeated STA failures before the first successful boot-time connection trigger AP fallback.

## License

This project is licensed under the MIT License.

You are free to download, use, modify, and distribute this code, including for commercial use, as long as the MIT license notice is kept with substantial portions of the software.

See [LICENSE](LICENSE) for the full text.
