#pragma once

#include <Arduino.h>
#include <WiFiManager.h>

#if defined(ESP8266)
    #include <ESP8266WiFi.h>
#elif defined(ESP32)
    #include <WiFi.h>
#endif

#include <functional>

namespace chrWiFi {

    // --- Enums ---
    enum Status {
        WIFI_OFF_STATUS = -1,
        WIFI_AP_MODE,
        WIFI_LOST,
        WIFI_WEAK,
        WIFI_MEDIUM,
        WIFI_STRONG
    };

    enum EventCode {
        EVENT_ERR = -10,
        EVENT_OTA_FAILED = -9,
        EVENT_UNSTABLE = -2,
        EVENT_WARN = -1,
        EVENT_OK = 0,
        EVENT_STABLE = 2,
        EVENT_NOTICE = 10,
        EVENT_OTA_PREPARE = 11,
        EVENT_STATUS = 12
    };

    using EventCallback = std::function<void(int8_t, const char*)>;

    // --- Public methods ---
    void setup(const char* apName = nullptr, const char* pass = nullptr, uint32_t statusCheckMs = 5387, uint32_t reconnectMs = 30000, uint16_t portalPort = 80);
    void startAP();
    void startSta();
    void stop();
    void startWebPortal();
    void stopWebPortal();

    Status checkStatus();
    Status currentStatus();
    Status loop();
    bool otaUpdateStarted();

    uint32_t ipToUint(const IPAddress &ip);
    IPAddress uintToIP(uint32_t v);

    IPAddress getIp();
    char* getApName();
    
    void onEvent(EventCallback cb);

    // --- WebServer methods ---

    // Add custom HTML to the portal
    void setCustomMenuHTML(const char* html);
    // Callback when the WiFiManager internal webserver is created / reset.
    void setWebServerCallback(std::function<void()> cb);
    // Webserver a további URL-ek működtetéséhez
    WiFiManager::WM_WebServer* webServer();
} // namespace chrWiFi
