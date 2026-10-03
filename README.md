# chrWiFi

`chrWiFi` wraps `tzapu/WiFiManager` with a non-blocking WiFi lifecycle for Arduino-based ESP devices. It can connect using saved station credentials, fall back to an access point, monitor connection state, and expose the WiFiManager web server for custom routes.

## Features

- Non-blocking WiFiManager processing through `chrWiFi::loop()`
- AP name generated from a base name and the device MAC suffix
- AP fallback when credentials are missing or initial STA attempts fail
- Optional saved-IP reuse after a successful DHCP connection
- Connection, IP, client-count, gateway-check, and portal events
- Optional web portal while connected in STA mode
- OTA preparation and failure notifications

## Dependency and Target

The library depends on `tzapu/WiFiManager` `^2.0.17` and uses the Arduino framework. The PlatformIO configuration in this repository builds the ESP32 `esp32dev` target.

## Quick Start

```cpp
#include "chrWiFi.h"

void onWiFiEvent(int8_t code) {
    const auto event = static_cast<chrWiFi::EventCode>(code);
    Serial.printf("event=%d (%s) status=%d\n",
                  code,
                  chrWiFi::eventName(event),
                  chrWiFi::currentStatus());
}

void setup() {
    Serial.begin(115200);

    // Register first to receive EVENT_INIT from setup().
    chrWiFi::setEventCallback(onWiFiEvent);
    chrWiFi::setup("chrWiFiDemo", "12345678");
    chrWiFi::startSta();
}

void loop() {
    chrWiFi::loop();
}
```

For a working example with event-specific status logging and custom HTTP handlers, see [examples/Basic/Basic.cpp](examples/Basic/Basic.cpp).

## Event Callback

Register a callback with `setEventCallback(EventCallback)`. It receives a single `int8_t` event code; no message buffer is allocated or copied. Convert it to `EventCode` when using the enum names, and use `eventName()` for a short label. Query the state you need from the callback using the getters below. Cached getters such as `currentIP()`, `currentStatus()`, and `currentConnectedCount()` return the value associated with the most recent status check; `getIP()`, `getStatus()`, and `getConnectedCount()` query immediately.

| Event | Code | State/details to query |
| --- | ---: | --- |
| `EVENT_ERR` | -10 | Reserved general error code; not currently emitted |
| `EVENT_WARN_OTA_FAILED` | -9 | The event code itself identifies an OTA failure |
| `EVENT_WARN_PORTAL` | -8 | `getPortalStatus()`; portal could not start because STA is not connected |
| `EVENT_WARN_MODE` | -7 | `currentStatus()`; mode fallback after repeated connection attempts |
| `EVENT_WARN_NO_SSID` | -6 | Saved station credentials were missing |
| `EVENT_WARN_IP_SAVE` | -5 | Static IP configuration could not be applied |
| `EVENT_UNSTABLE` | -2 | `currentStatus()` and `getGwCheckStatus()`; unstable connection transitions and gateway timeout |
| `EVENT_INIT` | 0 | Library initialization |
| `EVENT_STABLE` | 2 | `currentStatus()` and `getGwCheckStatus()`; stable STA/AP state or successful gateway check |
| `EVENT_NOTICE_DHCP` | 10 | `usingStaticIP()` distinguishes saved static IP from DHCP |
| `EVENT_NOTICE_GW` | 12 | `getGwCheckStatus()` gives the updated gateway-check state |
| `EVENT_NOTICE_SSID` | 14 | Saved network credentials changed; `getSSID()` reports the active WiFi SSID when connected |
| `EVENT_NOTICE_PORTAL` | 16 | `getPortalStatus()` gives the updated web portal state |
| `EVENT_NOTICE_RECONNECT` | 18 | A station reconnection attempt is starting |
| `EVENT_STATUS` | 20 | `currentStatus()` gives the new connection/signal status |
| `EVENT_OTA_PREPARE` | 25 | `otaUpdateStarted()` |
| `EVENT_IP` | 30 | `currentIP()` gives the changed address |
| `EVENT_CLIENTS` | 35 | `currentConnectedCount()` gives the updated AP client count |

Event names and numeric values changed from the previous API. Replace uses of the old generic `EVENT_WARN`, `EVENT_NOTICE`, `EVENT_OTA_FAILED`, and `EVENT_OK` values with the specific current event codes; do not rely on the previous numeric values.

Event codes and state getters replace the old free-form message strings. The state information remains available, but not every old explanatory phrase: for example, `EVENT_UNSTABLE` does not identify whether the caller was starting AP, starting STA, or stopping WiFi. `eventName()` returns event labels, not the former detailed messages.

Gateway results are updated before the corresponding stable/unstable callback. DHCP, gateway-check, and portal state are also updated before their respective notice events.

## Public API

### Setup and status

- `setup(const char* apName = nullptr, const char* pass = nullptr, uint32_t statusCheckMs = 5387, uint32_t reconnectMs = 30000, uint16_t portalPort = 80, bool gwCheck = true)` initializes the library and configures timing, AP credentials, portal port, and gateway checks.
- `getApName()` returns the generated AP name.
- `getIP()` queries the current IP immediately; `currentIP()` returns the last cached IP.
- `getConnectedCount()` queries the current AP client count immediately; `currentConnectedCount()` returns the last cached count.
- `getStatus()` queries the current WiFi status immediately; `currentStatus()` returns the last cached status.
- `usingStaticIP()` reports whether the saved static IP configuration is active.
- `getGwCheckStatus()` returns a `GWStateCode`.
- `getSSID()` returns the current WiFi SSID as an Arduino `String`.
- `getPortalStatus()` reports whether the web portal is active.
- `loop()` must be called repeatedly; it processes WiFiManager, reconnection, gateway checks, and scheduled status updates.
- `otaUpdateStarted()` reports whether OTA preparation is in progress.

### WiFi and portal control

- `startAP()` starts the non-blocking configuration portal in AP mode.
- `startSta(bool alwaysUseDHCP = true)` connects using saved WiFiManager credentials. When `alwaysUseDHCP` is false, a previously saved network configuration may be reused.
- `stop()` stops portal activity and turns off WiFi.
- `startWebPortal()` and `stopWebPortal()` control the web portal while in STA mode.
- `setCustomMenuHTML(const char* html)` adds custom HTML to the WiFiManager menu.
- `setWebServerCallback(std::function<void()> cb)` registers a callback for web server creation/reset.
- `webServer()` returns the underlying `WiFiManager::WM_WebServer*` for custom routes.

### Events

- `setEventCallback(EventCallback cb)` registers the single-argument callback.
- `eventName(EventCode code)` returns a short readable label for an event.

`Status` values are `WIFI_OFF_STATUS = -1`, `WIFI_AP_MODE = 0`, `WIFI_LOST = 1`, `WIFI_WEAK = 2`, `WIFI_MEDIUM = 3`, and `WIFI_STRONG = 4`. Connected STA signal levels are classified by RSSI: below -80 dBm is weak, below -60 dBm is medium, otherwise strong.

`GWStateCode` values are `GW_IN_PROGRESS = 0`, `GW_ALIVE = 1`, `GW_TIMEOUT = -1`, and `GW_NOT_RUNNING = -2`.

## Build

From the repository root, run:

```bash
platformio run
```

The default PlatformIO source directory is `examples/Basic`, so this builds the example and the library.

## Notes

- The default AP password is `12345678` when no password is provided.
- The AP name is `{base}{MAC[4]}{MAC[5]}`, for example `ESP-AP-1A2B`.
- Five unsuccessful STA attempts before the first connection trigger AP fallback.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
