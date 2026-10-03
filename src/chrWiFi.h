#pragma once

#include <Arduino.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>

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
        EVENT_WARN_OTA_FAILED = -9,
        EVENT_WARN_PORTAL = -8,
        EVENT_WARN_MODE = -7,
        EVENT_WARN_NO_SSID = -6,
        EVENT_WARN_IP_SAVE = -5,
        EVENT_UNSTABLE = -2,
        EVENT_INIT = 0,
        EVENT_STABLE = 2,
        EVENT_NOTICE_DHCP = 10,
        EVENT_NOTICE_GW = 12,
        EVENT_NOTICE_SSID = 14,
        EVENT_NOTICE_PORTAL = 16,
        EVENT_NOTICE_RECONNECT = 18,
        EVENT_STATUS = 20,
        EVENT_OTA_PREPARE = 25,
        EVENT_IP = 30,
        EVENT_CLIENTS = 35
    };

    enum GWStateCode {
        GW_IN_PROGRESS = 0,
        GW_ALIVE = 1,
        GW_TIMEOUT = -1,
        GW_NOT_RUNNING = -2
    };

    // --- Callbacks ---
    using EventCallback = std::function<void(int8_t)>;

    // --- Public methods ---
    void setup(const char* apName = nullptr, const char* pass = nullptr, uint32_t statusCheckMs = 5387, uint32_t reconnectMs = 30000, uint16_t portalPort = 80, bool gwCheck = true);

    const char* getApName();
    IPAddress getIP();                  // Checks immediately and only returns current IP (currentIP() will be refreshed only on next schedule!)
    IPAddress currentIP();              // Returns the last known IP without checking immediately
    uint8_t getConnectedCount();        // Checks immediately and only returns current connected count (currentConnectedCount() will be refreshed only on next schedule!)
    uint8_t currentConnectedCount();    // Returns the last known connected count without checking immediately
    Status getStatus();                 // Checks immediately and only returns current status (currentstatus() will be refreshed only on next schedule!)
    Status currentStatus();             // Returns the last known status without checking immediately
    bool usingStaticIP();
    GWStateCode getGwCheckStatus();
    String getSSID();
    bool getPortalStatus();

    void startAP();
    void startSta(bool alwaysUseDHCP = true);
    void stop();
    void startWebPortal();
    void stopWebPortal();

    Status loop();
    bool otaUpdateStarted();

    void setEventCallback(EventCallback cb);
    const char* eventName(EventCode code);

    // --- WebServer methods ---

    // Add custom HTML to the portal
    void setCustomMenuHTML(const char* html);
    // Callback when the WiFiManager internal webserver is created / reset.
    void setWebServerCallback(std::function<void()> cb);
    // Webserver to manage additional URLs and handle requests
    WiFiManager::WM_WebServer* webServer();
} // namespace chrWiFi
