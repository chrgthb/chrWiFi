/*
 * chrWiFi example sketch
 *
 * This example shows how to use the chrWiFi namespace for robust ESP WiFi
 * setup, event-driven status monitoring, and a small web portal that can be
 * started while the device is already operating in STA mode.
 *
 * It is useful for devices that need:
 *   - WiFi connection management with fallback to AP mode
 *   - serial event logging for debugging and status changes
 *   - simple GET and POST API endpoints
 *   - a static HTML page served from the local web server
 */

#include "chrWiFi.h"

// Demo state for the simple API.
bool ledOn = false;

inline const char INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>chrWiFi Demo</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      background: #121826;
      color: #e5eefb;
      display: grid;
      place-items: center;
      min-height: 100vh;
      margin: 0;
    }
    .card {
      width: min(480px, 92vw);
      background: #1d2437;
      border-radius: 12px;
      padding: 24px;
      box-shadow: 0 16px 36px rgba(0, 0, 0, 0.25);
    }
    h1 {
      margin-top: 0;
    }
    .status {
      display: inline-block;
      padding: 8px 12px;
      border-radius: 999px;
      background: #2f7ef7;
      color: white;
      font-weight: bold;
    }
    button {
      border: none;
      border-radius: 8px;
      padding: 12px 18px;
      font-size: 1rem;
      cursor: pointer;
      margin-right: 10px;
    }
    .on { background: #2ecc71; color: white; }
    .off { background: #e74c3c; color: white; }
    .small { margin-top: 16px; color: #bfd5ff; }
  </style>
</head>
<body>
  <div class="card">
    <h1>chrWiFi Demo</h1>
    <div class="status">LED: <span id="ledState">OFF</span></div>
    <p>This page is served locally by the chrWiFi web portal.</p>
    <button class="on" onclick="fetch('/api/led', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({ state: 'on' }) }).then(r => r.json()).then(d => { document.getElementById('ledState').textContent = d.state.toUpperCase(); })">Turn ON</button>
    <button class="off" onclick="fetch('/api/led', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({ state: 'off' }) }).then(r => r.json()).then(d => { document.getElementById('ledState').textContent = d.state.toUpperCase(); })">Turn OFF</button>
    <div class="small">Try: <a href="/api/led">GET /api/led</a> or <a href="/api/info">GET /api/info</a></div>
  </div>
</body>
</html>
)html";

void registerWebHandlers() {
  static WiFiManager::WM_WebServer* boundServer = nullptr;
  if (chrWiFi::webServer() == nullptr || chrWiFi::webServer() == boundServer) {
    Serial.println("[chrWiFi] Web server pointer is not ready yet or already bound.");
    return;
  }
  boundServer = chrWiFi::webServer();


  // Simple static page.
  chrWiFi::webServer()->on("/", HTTP_GET, []() {
    chrWiFi::webServer()->send(200, "text/html", INDEX_HTML);
  });

  // Simple GET endpoint returning the current LED state.
  chrWiFi::webServer()->on("/api/led", HTTP_GET, []() {
    String state = ledOn ? "on" : "off";
    chrWiFi::webServer()->send(200, "application/json", String("{\"state\":\"") + state + "\"}");
  });

  // Simple POST endpoint that accepts a JSON body and updates the LED state.
  chrWiFi::webServer()->on("/api/led", HTTP_POST, []() {
    String payload = chrWiFi::webServer()->arg("plain");
    String state = "off";

    if (payload.length() > 0) {
      if (payload.indexOf("\"on\"") >= 0 || payload.indexOf("\"true\"") >= 0) {
        ledOn = true;
        state = "on";
      } else if (payload.indexOf("\"off\"") >= 0 || payload.indexOf("\"false\"") >= 0) {
        ledOn = false;
        state = "off";
      }
    }

    chrWiFi::webServer()->send(200, "application/json", String("{\"state\":\"") + state + "\"}");
  });

  // Another example GET endpoint returning an informational payload.
  chrWiFi::webServer()->on("/api/info", HTTP_GET, []() {
    String ip = WiFi.localIP().toString();
    String response = String("{\"ssid\":\"") + WiFi.SSID() + "\",\"ip\":\"" + ip + "\",\"status\":\"ok\"}";
    chrWiFi::webServer()->send(200, "application/json", response);
  });

  // Optional 404 handler.
  chrWiFi::webServer()->onNotFound([]() {
    chrWiFi::webServer()->send(404, "text/plain", "Not found");
  });

  Serial.println("[chrWiFi] Web handlers registered.");
}

void handleWiFiEvent(int8_t code, const char* msg) {
  Serial.printf("[chrWiFi] event=%d msg=%s\n", code, (msg != nullptr) ? msg : "null");

  // When the module is in STA mode and connected, start the portal/web server.
  // This is the easiest way to publish a small control page while the device is online.
  // In AP mode the portal starts automatically, so no need to handle that case here.
  if (code == chrWiFi::EVENT_STATUS) {
    chrWiFi::Status status = chrWiFi::getStatus();

    if (status > chrWiFi::WIFI_LOST) {
      Serial.println("[chrWiFi] STA connected. Starting web portal...");
      chrWiFi::startWebPortal();
      registerWebHandlers();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== chrWiFi example ===");
  Serial.println("The library handles WiFi mode selection, event callbacks, and web portal setup.");

  // Basic init. The AP name is built automatically from a base name plus MAC suffix.
  // The default password is also accepted as a fallback for the config portal.
  chrWiFi::setup("chrWiFiDemo", "12345678", 5000, 15000, 80);

  // Register a global event callback to print all library status messages.
  chrWiFi::setEventCallback(handleWiFiEvent);

  // Add a small custom HTML fragment to the WiFiManager portal if needed.
  chrWiFi::setCustomMenuHTML("<p><b>chrWiFi Demo</b> - simple WiFi + web portal</p>");

  // Add a callback to register your own web handlers when the internal web server is ready.
  chrWiFi::setWebServerCallback(registerWebHandlers);

  // Attempt to connect to saved WiFi credentials in STA mode.
  // If nothing is saved yet, the library automatically falls back to AP mode.
  chrWiFi::startSta();
}

void loop() {
  // This should be called repeatedly to process WiFi status changes and reconnect logic.
  chrWiFi::loop();

  // Optional: if you want a very small periodic heartbeat on the serial console.
  static uint32_t lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 30000) {
    lastHeartbeat = millis();
    Serial.printf("[chrWiFi] current status=%d IP=%s\n",
                  chrWiFi::getStatus(),
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "0.0.0.0");
  }
}
