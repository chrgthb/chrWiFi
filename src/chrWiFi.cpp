#include "chrWiFi.h"

#if defined(ESP32)
    #include <esp_mac.h>
#endif

namespace chrWiFi {
  
    // --- Private vars ---
    WiFiManager _wm;
    bool _initOk = false;
    char _apName[32] = "";
    char _apPass[32] = "12345678";
    // Timing vars
    uint32_t _statusCheckInterval = 5387; // Prime number try to avoid sync with other timers
    uint32_t _lastStatusCheck = 0;
    uint16_t _portalPort = 80;
    uint32_t _reconnectInterval = 30000; 
    uint32_t _lastReconnectAttempt = 0;
    // Status vars
    Status _currentStatus = WIFI_OFF_STATUS;
    bool _shouldBeConnected = false;
    bool _staConnectedSinceBoot = false;
    uint8_t _staAttemptCount = 0;
    constexpr uint8_t _maxStaAttemptsBeforeApFallback = 5;
    // OTA vars
    bool _otaUpdateStarted = false;
    bool _otaUpdateFailed = false;
    // Event callback
    EventCallback _onEvent = nullptr;
    // No DHCP vars
    bool _runningOnStaticIP = false;
    const uint32_t VALID_DATA_MAGIC = 0xAA55AA55;
    struct _NetConfig {
        uint32_t ip = 0;
        uint32_t gateway = 0;
        uint32_t netmask = 0;
        uint32_t magic = VALID_DATA_MAGIC;
    };
    _NetConfig _configData;
#if defined(ESP32)
    RTC_DATA_ATTR _NetConfig rtcConfig;
#endif
    // Gateway checking vars
    bool _gwCheck = true;
    WiFiUDP _udp;
    unsigned long _gwCheckStartTime = 0;
    int8_t _gwCheckStatus = -2;             // 0 = in progress, 1 = GW alive, -1 = timeout, -2 = not running
    // - minimal DNS query for the root "." server
    const uint8_t _dnsQuery[] = {
        0xAA, 0xAA, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01
    };


    // --- Methods ---
    void setEventCallback(EventCallback cb) { _onEvent = cb; }

    void _fireEvent(int8_t code, const char* msg) {
        if (_onEvent) _onEvent(code, msg);
    }

    // No DHCP methods
    bool _saveNetworkToRTCIfChanged(uint32_t ip, uint32_t gw, uint32_t mask) {
        if (_runningOnStaticIP) return false;

        if (ip == 0 || gw == 0 || mask == 0) return false;   // Incorrect network config

        bool res = _configData.ip != ip || _configData.gateway != gw || _configData.netmask != mask;
        if (!res) return false; // No change

        _configData.ip = ip;
        _configData.gateway = gw;
        _configData.netmask = mask;
        _configData.magic = VALID_DATA_MAGIC;

#if defined(ESP32)
        rtcConfig = _configData;
#elif defined(ESP8266)
        ESP.rtcUserMemoryWrite(0, (uint32_t*)&_configData, sizeof(_configData));
#endif

        return res;
    }

    bool _loadNetworkFromRTC() {
#if defined(ESP32)
        _configData = rtcConfig;
#elif defined(ESP8266)
        ESP.rtcUserMemoryRead(0, (uint32_t*)&_configData, sizeof(_configData));
#endif

        return _configData.magic == VALID_DATA_MAGIC;
    }

    static bool _applyStaticConfig(const _NetConfig& cfg) {
        bool res = WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.netmask));
        if (res) {
            _runningOnStaticIP = true;
            _fireEvent(EVENT_NOTICE, "STA using saved IP config");
        } else {
            _fireEvent(EVENT_WARN, "STA failed to apply saved IP config");
        }
        return res;
    }

    static void _applyDhcpConfig() {
        IPAddress zero(0, 0, 0, 0);
        WiFi.config(zero, zero, zero);
        _runningOnStaticIP = false;
        _fireEvent(EVENT_NOTICE, "STA using DHCP");
    }

    // MAC methods
    static void _getStableStaMac(uint8_t mac[6]) {
#if defined(ESP32)
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
#else
        wifi_mode_t previousMode = WiFi.getMode();
        if (previousMode == WIFI_OFF) {
            WiFi.mode(WIFI_STA);
        }
        WiFi.macAddress(mac);
        if (previousMode == WIFI_OFF) {
            WiFi.mode(WIFI_OFF);
        }
#endif
    }

    // Stable/unstable event deduplication
    static bool _lastStable = false;
    static void _notifyStable(bool stable, const char* msg) {
        if (stable == _lastStable) return;
        _lastStable = stable;
        if (stable) _fireEvent(EVENT_STABLE, msg);
        else _fireEvent(EVENT_UNSTABLE, msg);
    }

    static bool _canFallbackToAp() {
        return !_staConnectedSinceBoot && _staAttemptCount >= _maxStaAttemptsBeforeApFallback;
    }

    static void _countStaAttempt() {
        if (!_staConnectedSinceBoot && _staAttemptCount < _maxStaAttemptsBeforeApFallback) {
            ++_staAttemptCount;
        }
    }

    static void _beginStaConnect(const char* ssid, const char* pass) {
        _countStaAttempt();
        WiFi.begin(ssid, pass);
    }

    static void _beginStaConnect() {
        _countStaAttempt();
        WiFi.begin();
    }

    // Network testing methods

    // GW check with DNS query to root server
    void _startGwCheck() {
        if (!_gwCheck) return;    // GW check disabled
        if (_gwCheckStatus == 0) return;    // Already in progress
        _fireEvent(EVENT_NOTICE, "checking GW...");

        _udp.begin(8888);   // Just a random port
        _udp.beginPacket(WiFi.gatewayIP(), 53);
        _udp.write(_dnsQuery, sizeof(_dnsQuery));
        _udp.endPacket();   // UDP query send ARP if needed

        _gwCheckStartTime = millis();
        _gwCheckStatus = 0;
    }
    void _stopGwCheck() {
        if (!_gwCheck) return;    // GW check disabled
        if (_gwCheckStatus == -2) return;    // Not running, nothing to stop
        _fireEvent(EVENT_NOTICE, "stopping GW check...");

        _udp.stop();
        _gwCheckStatus = -2;
    }

    void _loopGwCheck() {
        if (!_gwCheck) return;    // GW check disabled
        if (_gwCheckStatus != 0) return;    // Only proceed if GW check is in progress

        if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) {
            // Maybe the current status changed
            _stopGwCheck();
            return;
        }

        // Check if data received
        if (_udp.parsePacket() > 0) {
            _notifyStable(true, "GW is alive");
            _udp.stop();

            _gwCheckStatus = 1;
            return;
        }

        // Check timeout (500ms should be more than enough)
        if (millis() - _gwCheckStartTime > 500) {
            _udp.stop();
            _notifyStable(false, "GW check timed out");

            if (_runningOnStaticIP) {
                // Retry STA connection with DHCP configuration
                _applyDhcpConfig();
                _lastReconnectAttempt = millis();
                _beginStaConnect();
            }

            _gwCheckStatus = -1;
            return;
        }
    }

    static void _onPreOtaUpdate() {
        _otaUpdateStarted = true;
        _otaUpdateFailed = false;
        _fireEvent(EVENT_OTA_PREPARE, "OTA update started");
    }

    void setup(const char* apName, const char* pass, uint32_t statusCheckMs, uint32_t reconnectMs, uint16_t portalPort, bool gwCheck) {
        _fireEvent(EVENT_NOTICE, "init...");

        _gwCheck = gwCheck;

        WiFi.mode(WIFI_OFF);
        _currentStatus = WIFI_OFF_STATUS;
        _shouldBeConnected = false;
        
        if (pass) {
            snprintf(_apPass, sizeof(_apPass), "%s", pass);
        }
        _statusCheckInterval = statusCheckMs;
        _reconnectInterval = reconnectMs;
        _portalPort = portalPort;

        // Get stabil MAC address to generate a unique AP name
        uint8_t mac[6];
        _getStableStaMac(mac);
        // AP name with MAC suffix, e.g., "ESP-AP-1A2B"
        snprintf(_apName, sizeof(_apName), "%s%02X%02X", apName ? apName : "ESP-AP", mac[4], mac[5]);
        
        _wm.setHttpPort(_portalPort);
        _wm.setConfigPortalBlocking(false);

        // STA mode connect timeout: 30s
        _wm.setConnectTimeout(30);
        _wm.setPreOtaUpdateCallback(_onPreOtaUpdate);

        _otaUpdateStarted = false;
        _otaUpdateFailed = false;

        _initOk = true;
        
    }
    
    Status checkStatus() {
        if (WiFi.getMode() == WIFI_OFF) return WIFI_OFF_STATUS;
        if (WiFi.getMode() & WIFI_AP) return WIFI_AP_MODE;
        if (WiFi.status() != WL_CONNECTED) return WIFI_LOST;
        
        int32_t rssi = WiFi.RSSI();
        if (rssi < -80) return WIFI_WEAK;
        if (rssi < -60) return WIFI_MEDIUM;
        return WIFI_STRONG;
    }

    Status getStatus() {
        return _currentStatus;
    }

    void startAP() {
        _shouldBeConnected = false;
        if (!_wm.getConfigPortalActive()) {
            _notifyStable(false, "start AP...");
            if (!_initOk) setup();

            _wm.startConfigPortal(_apName, _apPass);
            
            // Change status immediately
            _currentStatus = checkStatus();
        }
    }

    void startSta(bool alwaysUseDHCP) {
        if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) { return; }

        WiFi.mode(WIFI_STA);
        delay(10);

        String savedSsid = _wm.getWiFiSSID();
        String savedPass = _wm.getWiFiPass();

        if (savedSsid.length() == 0) {
            // No saved credentials, fallback to AP
            startAP();
            return;
        }

        if (_canFallbackToAp()) {
            _fireEvent(EVENT_WARN, "STA failed repeatedly, switching to AP");
            startAP();
            return;
        }

        if (!alwaysUseDHCP && _loadNetworkFromRTC()) {
            bool staticIPApplied =_applyStaticConfig(_configData);
            if (!staticIPApplied) _applyDhcpConfig();
        } else {
            _applyDhcpConfig();
        }
        
        _notifyStable(false, "start STA...");
        if (!_initOk) setup();
        
        _shouldBeConnected = true;
        _lastReconnectAttempt = millis();

        _beginStaConnect(savedSsid.c_str(), savedPass.c_str());
        
        // Change status immediately
        _currentStatus = checkStatus();
    }

    void stop() {
        _shouldBeConnected = false; // Stop autoreconnect
        
        if (WiFi.getMode() == WIFI_OFF) { return; }
        _notifyStable(false, "stop...");
        
        // Stop config portal if active
        if (_wm.getConfigPortalActive()) {
            _wm.stopConfigPortal();
        }
        stopWebPortal();

        WiFi.disconnect(true); // true = turn off radio
        WiFi.mode(WIFI_OFF);
        
        // Change status immediately
        _currentStatus = checkStatus();
    }

    void startWebPortal() {
        if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) {
            _fireEvent(EVENT_WARN, "portal start failed: not STA mode / not connected");
            return;
        }

        if (!_wm.getWebPortalActive()) {
            _fireEvent(EVENT_NOTICE, "start portal...");
            _wm.startWebPortal(); 
        }
    }

    void stopWebPortal() {
        if (_wm.getWebPortalActive()) {
            _fireEvent(EVENT_NOTICE, "stop portal...");
            _wm.stopWebPortal();
        }
        
    }

    Status loop() {
        if (!_initOk) setup();
        
        _wm.process();

        if (_otaUpdateStarted) {
            if (!_otaUpdateFailed && Update.hasError()) {
                _otaUpdateFailed = true;
                _otaUpdateStarted = false;
                _fireEvent(EVENT_OTA_FAILED, "OTA update failed");
            }
            _stopGwCheck();
            return _currentStatus;
        }

        uint32_t now = millis();
        _loopGwCheck();

        // Autoreconnect logika
        if (_shouldBeConnected && WiFi.status() != WL_CONNECTED && !_wm.getConfigPortalActive()) {
            if (now - _lastReconnectAttempt >= _reconnectInterval) {
                _lastReconnectAttempt = now;

                if (_canFallbackToAp()) {
                    _fireEvent(EVENT_WARN, "STA failed repeatedly, switching to AP");
                    startAP();
                }
                else {
                _fireEvent(EVENT_NOTICE, "reconnect...");
                WiFi.mode(WIFI_STA);
                _beginStaConnect(); 
                }
            }
        }

        // Status check
        if (now - _lastStatusCheck >= _statusCheckInterval) {
            _lastStatusCheck = now;
            Status oldStatus = _currentStatus;
            _currentStatus = checkStatus();

            if (oldStatus != _currentStatus) {
                // New status
                char msg[24] = "";
                
                switch (_currentStatus) {
                    case WIFI_STRONG:
                        snprintf_P(msg, sizeof(msg), PSTR("signal STRONG"));
                        _staConnectedSinceBoot = true;
                        _notifyStable(true, "STA stable");
                        break;
                    case WIFI_MEDIUM:
                        snprintf_P(msg, sizeof(msg), PSTR("signal MEDIUM"));
                        _staConnectedSinceBoot = true;
                        _notifyStable(true, "STA stable");
                        break;
                    case WIFI_WEAK:
                        snprintf_P(msg, sizeof(msg), PSTR("signal WEAK"));
                        _staConnectedSinceBoot = true;
                        _notifyStable(true, "STA stable");
                        break;
                    case WIFI_LOST:
                        snprintf_P(msg, sizeof(msg), PSTR("signal LOST"));
                        if (oldStatus != WIFI_OFF_STATUS) {
                            // There was a connection before, now it's lost
                            _notifyStable(false, "connection lost");
                        }
                        break;
                    case WIFI_AP_MODE:
                        _runningOnStaticIP = false;
                        snprintf_P(msg, sizeof(msg), PSTR("mode: AP"));
                        _notifyStable(true, "AP stable");
                        break;
                    case WIFI_OFF_STATUS:
                        _runningOnStaticIP = false;
                        snprintf_P(msg, sizeof(msg), PSTR("mode: OFF"));
                        break;
                }
                _fireEvent(EVENT_STATUS, msg);

                if (_currentStatus > WIFI_LOST) {
                    _saveNetworkToRTCIfChanged((uint32_t)WiFi.localIP(), (uint32_t)WiFi.gatewayIP(), (uint32_t)WiFi.subnetMask());
                    _startGwCheck();
                } else {
                    _stopGwCheck();
                }
            }
        }

        return _currentStatus;
    }

    bool otaUpdateStarted() {
        return _otaUpdateStarted;
    }

    IPAddress getIp() {
        IPAddress ip = IPAddress(0,0,0,0);

        if (WiFi.status() == WL_CONNECTED) {
            // Client mode
            ip = WiFi.localIP();
        } else if (WiFi.getMode() & WIFI_AP) {
            // AP mode
            ip = WiFi.softAPIP();
        }

        char msg[24];
        snprintf_P(msg, sizeof(msg), PSTR("IP: %u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
        _fireEvent(EVENT_NOTICE, msg);
        return ip;
    }

    char* getApName() {
        return _apName;
    }

    void setCustomMenuHTML(const char* html) {
        _wm.setCustomMenuHTML(html);
    }

    void setWebServerCallback(std::function<void()> cb) {
        _wm.setWebServerCallback(cb);
    }

    WiFiManager::WM_WebServer* webServer() {
        return _wm.server.get();
    }

} // namespace chrWiFi