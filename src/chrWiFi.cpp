#include "chrWiFi.h"

#if defined(ESP32)
    #include <esp_mac.h>
#endif

namespace chrWiFi {
  
    // --- Private vars ---
    namespace {
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
        constexpr uint8_t _MAX_RECONNECT_BEFORE_AP_FALLBACK = 5;
        IPAddress _currentIP;
        uint8_t _connectedCount = 0;
        // OTA vars
        bool _otaUpdateStarted = false;
        bool _otaUpdateFailed = false;
        // Event callback
        EventCallback _onEvent = nullptr;
        // No DHCP vars
        bool _runningOnStaticIP = false;
        constexpr uint32_t _VALID_DATA_MAGIC = 0xAA55AA55;
        struct _NetConfig {
            uint32_t ip = 0;
            uint32_t gateway = 0;
            uint32_t netmask = 0;
            uint32_t ssidHash = 0;
            uint32_t magic = _VALID_DATA_MAGIC;
        };
        _NetConfig _configData;
#if defined(ESP32)
        RTC_DATA_ATTR _NetConfig rtcConfig;
#endif
        // Gateway checking vars
        bool _gwCheck = true;
        WiFiUDP _udp;
        unsigned long _gwCheckStartTime = 0;
        GWStateCode _gwCheckStatus = GW_NOT_RUNNING;
        // - minimal DNS query for the root "." server
        constexpr uint8_t _DNS_QUERY[] = {
            0xAA, 0xAA, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01
        };
        
        // --- Private methods ---
        void _fireEvent(int8_t code) {
            if (_onEvent) _onEvent(code);
        }
        
        // No DHCP methods
        uint32_t _hashSsid(const char* ssid) {
            // FNV-1a (32-bit), simple and deterministic for small strings.
            uint32_t hash = 2166136261u;
            if (!ssid) return hash;
    
            while (*ssid) {
                hash ^= (uint8_t)(*ssid++);
                hash *= 16777619u;
            }
            return hash;
        }
    
        void _clearNetworkFromRTC() {
            _configData.ip = 0;
            _configData.gateway = 0;
            _configData.netmask = 0;
            _configData.ssidHash = 0;
            _configData.magic = 0;

#if defined(ESP32)
            rtcConfig = _configData;
#elif defined(ESP8266)
            ESP.rtcUserMemoryWrite(0, (uint32_t*)&_configData, sizeof(_configData));
#endif
        }
        
        bool _saveNetworkToRTCIfChanged(uint32_t ip, uint32_t gw, uint32_t mask, const char* ssid) {
            if (_runningOnStaticIP) return false;
    
            if (ip == 0 || gw == 0 || mask == 0) return false;   // Incorrect network config
    
            uint32_t ssidHash = _hashSsid(ssid);
    
            bool res = _configData.ip != ip
                || _configData.gateway != gw
                || _configData.netmask != mask
                || _configData.ssidHash != ssidHash;
            if (!res) return false; // No change
    
            _configData.ip = ip;
            _configData.gateway = gw;
            _configData.netmask = mask;
            _configData.ssidHash = ssidHash;
            _configData.magic = _VALID_DATA_MAGIC;

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
    
            return _configData.magic == _VALID_DATA_MAGIC;
        }
        
        bool _applyStaticConfig(const _NetConfig& cfg) {
            bool res = WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.netmask));
            if (res) {
                _runningOnStaticIP = true;
                _fireEvent(EVENT_NOTICE_DHCP);
            } else {
                _fireEvent(EVENT_WARN_IP_SAVE);
            }
            return res;
        }
    
        void _applyDhcpConfig() {
            IPAddress zero(0, 0, 0, 0);
            WiFi.config(zero, zero, zero);
            _runningOnStaticIP = false;
            _fireEvent(EVENT_NOTICE_DHCP);
        }
        
        // MAC methods
        void _getStableStaMac(uint8_t mac[6]) {
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
        bool _lastStable = false;
        void _notifyStable(bool stable) {
            if (stable == _lastStable) return;
            _lastStable = stable;
            if (stable) _fireEvent(EVENT_STABLE);
            else _fireEvent(EVENT_UNSTABLE);
        }
    
        bool _canFallbackToAp() {
            return !_staConnectedSinceBoot && _staAttemptCount >= _MAX_RECONNECT_BEFORE_AP_FALLBACK;
        }
    
        void _countStaAttempt() {
            if (!_staConnectedSinceBoot && _staAttemptCount < _MAX_RECONNECT_BEFORE_AP_FALLBACK) {
                ++_staAttemptCount;
            }
        }
    
        void _beginStaConnect(const char* ssid, const char* pass) {
            _countStaAttempt();
            WiFi.begin(ssid, pass);
        }
    
        void _beginStaConnect() {
            _countStaAttempt();
            WiFi.begin();
        }
        
        // Network testing methods
    
        // GW check with DNS query to root server
        void _startGwCheck() {
            if (!_gwCheck) return;    // GW check disabled
            if (_gwCheckStatus == GW_IN_PROGRESS) return;    // Already in progress
    
            _udp.begin(8888);   // Just a random port
            _udp.beginPacket(WiFi.gatewayIP(), 53);
            _udp.write(_DNS_QUERY, sizeof(_DNS_QUERY));
            _udp.endPacket();   // UDP query send ARP if needed
    
            _gwCheckStartTime = millis();
            _gwCheckStatus = GW_IN_PROGRESS;
            _fireEvent(EVENT_NOTICE_GW);
        }
        void _stopGwCheck() {
            if (!_gwCheck) return;    // GW check disabled
            if (_gwCheckStatus == GW_NOT_RUNNING) return;    // Not running, nothing to stop
    
            _udp.stop();
            _gwCheckStatus = GW_NOT_RUNNING;
            _fireEvent(EVENT_NOTICE_GW);
        }
        
        void _loopGwCheck() {
            if (!_gwCheck) return;    // GW check disabled
            if (_gwCheckStatus != GW_IN_PROGRESS) return;    // Only proceed if GW check is in progress
    
            if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) {
                // Maybe the current status changed
                _stopGwCheck();
                return;
            }
    
            // Check if data received
            if (_udp.parsePacket() > 0) {
                _udp.stop();
                _gwCheckStatus = GW_ALIVE;
                _notifyStable(true);
                return;
            }
    
            // Check timeout (500ms should be more than enough)
            if (millis() - _gwCheckStartTime > 500) {
                _udp.stop();
                _gwCheckStatus = GW_TIMEOUT;
    
                if (_runningOnStaticIP) {
                    // Retry STA connection with DHCP configuration
                    _applyDhcpConfig();
                    _lastReconnectAttempt = millis();
                    _beginStaConnect();
                }
    
                _notifyStable(false);
                return;
            }
        }
        
        void _onPreOtaUpdate() {
            _otaUpdateStarted = true;
            _otaUpdateFailed = false;
            _fireEvent(EVENT_OTA_PREPARE);
        }
    }

    // --- Public methods ---
    void setEventCallback(EventCallback cb) { _onEvent = cb; }

    const char* eventName(EventCode code) {
    switch (code) {
        case EVENT_ERR:                 return "error";
        case EVENT_WARN_OTA_FAILED:     return "OTA failed";
        case EVENT_WARN_PORTAL:         return "portal failed: not connected";
        case EVENT_WARN_MODE:           return "mode switch AP/STA";
        case EVENT_WARN_NO_SSID:        return "STA warning: no SSID";
        case EVENT_WARN_IP_SAVE:        return "IP save failed";
        case EVENT_UNSTABLE:            return "unstable";
        case EVENT_INIT:                return "init";
        case EVENT_STABLE:              return "stable";
        case EVENT_NOTICE_DHCP:         return "DHCP/static status";
        case EVENT_NOTICE_GW:           return "GW check status";
        case EVENT_NOTICE_SSID:         return "SSID changed";
        case EVENT_NOTICE_PORTAL:       return "Portal status";
        case EVENT_NOTICE_RECONNECT:    return "reconnecting";
        case EVENT_STATUS:              return "status changed";
        case EVENT_IP:                  return "IP changed";
        case EVENT_CLIENTS:             return "clients status";
    }
    return "unknown";
}

    void setup(const char* apName, const char* pass, uint32_t statusCheckMs, uint32_t reconnectMs, uint16_t portalPort, bool gwCheck) {
        _fireEvent(EVENT_INIT);

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

    const char* getApName() {
        return _apName;
    }

    IPAddress getIP() {
        IPAddress ip = IPAddress(0,0,0,0);

        if (WiFi.status() == WL_CONNECTED) {
            // Client mode
            ip = WiFi.localIP();
        } else if (WiFi.getMode() & WIFI_AP) {
            // AP mode
            ip = WiFi.softAPIP();
        }

        return ip;
    }

    IPAddress currentIP() {
        return _currentIP;
    }

    uint8_t getConnectedCount() {
        return WiFi.softAPgetStationNum();
    }

    uint8_t currentConnectedCount() {
        return _connectedCount;
    }

    Status getStatus() {
        if (WiFi.getMode() == WIFI_OFF) return WIFI_OFF_STATUS;
        if (WiFi.getMode() & WIFI_AP) return WIFI_AP_MODE;
        if (WiFi.status() != WL_CONNECTED) return WIFI_LOST;
        
        int32_t rssi = WiFi.RSSI();
        if (rssi < -80) return WIFI_WEAK;
        if (rssi < -60) return WIFI_MEDIUM;
        return WIFI_STRONG;
    }

    Status currentStatus() {
        return _currentStatus;
    }
    
    bool usingStaticIP() {
        return _runningOnStaticIP;
    }

    GWStateCode getGwCheckStatus() {
        return _gwCheckStatus;
    }

    String getSSID() {
        return WiFi.SSID();
    }

    bool getPortalStatus() {
        return _wm.getWebPortalActive();
    }

    namespace {
        // Checks status and IP, notifies stable/unstable states, starts/stops GW check, saves new STA IP, count connected clients in AP mode
        static void _checkStatus(uint32_t time) {
            _lastStatusCheck = time;
            Status oldStatus = _currentStatus;
            _currentStatus = getStatus();
    
            // Status change check
            if (oldStatus != _currentStatus) {
                switch (_currentStatus) {
                    case WIFI_STRONG:
                        _staConnectedSinceBoot = true;
                        _notifyStable(true);
                        break;
                    case WIFI_MEDIUM:
                        _staConnectedSinceBoot = true;
                        _notifyStable(true);
                        break;
                    case WIFI_WEAK:
                        _staConnectedSinceBoot = true;
                        _notifyStable(true);
                        break;
                    case WIFI_LOST:
                        if (oldStatus != WIFI_OFF_STATUS) {
                            // There was a connection before, now it's lost
                            _notifyStable(false);
                        }
                        break;
                    case WIFI_AP_MODE:
                        _runningOnStaticIP = false;
                        _notifyStable(true);
                        break;
                    case WIFI_OFF_STATUS:
                        _runningOnStaticIP = false;
                        break;
                }
                _fireEvent(EVENT_STATUS);
            }
    
            // IP change check
            IPAddress oldIP = _currentIP;
            _currentIP = getIP();
            if (oldIP != _currentIP) _fireEvent(EVENT_IP);
    
            // IP saving and GW check start/stop
            if (_currentStatus > WIFI_LOST
                && (
                    (oldStatus != _currentStatus)
                    ||
                    (oldIP != _currentIP)
                )
            ) {
                _saveNetworkToRTCIfChanged((uint32_t)_currentIP, (uint32_t)WiFi.gatewayIP(), (uint32_t)WiFi.subnetMask(), WiFi.SSID().c_str());
                _startGwCheck();
            } else {
                _stopGwCheck();
            }
    
            // Count AP connected clients
            if (_currentStatus == WIFI_AP_MODE) {
                uint8_t oldConnectedCount = _connectedCount;
                _connectedCount = getConnectedCount();
    
                if (oldConnectedCount != _connectedCount) _fireEvent(EVENT_CLIENTS);
            }
        }
    }

    void startAP() {
        _shouldBeConnected = false;
        if (!_wm.getConfigPortalActive()) {
            _notifyStable(false);
            if (!_initOk) setup();

            _wm.startConfigPortal(_apName, _apPass);
            
            // Change status immediately
            _checkStatus(millis());
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
            _fireEvent(EVENT_WARN_NO_SSID);
            startAP();
            return;
        }

        if (_canFallbackToAp()) {
            _fireEvent(EVENT_WARN_MODE);
            startAP();
            return;
        }

        if (!alwaysUseDHCP && _loadNetworkFromRTC()) {
            uint32_t currentSsidHash = _hashSsid(savedSsid.c_str());

            // If credentials now point to a different SSID, discard stale static config.
            if (_configData.ssidHash != 0 && _configData.ssidHash != currentSsidHash) {
                _clearNetworkFromRTC();
                _fireEvent(EVENT_NOTICE_SSID);
                _applyDhcpConfig();
            } else {
                bool staticIPApplied = _applyStaticConfig(_configData);
                if (!staticIPApplied) _applyDhcpConfig();
            }
        } else {
            _applyDhcpConfig();
        }
        
        _notifyStable(false);
        if (!_initOk) setup();
        
        _shouldBeConnected = true;
        _lastReconnectAttempt = millis();

        _beginStaConnect(savedSsid.c_str(), savedPass.c_str());
        
        // Change status immediately
        _checkStatus(millis());
    }

    void stop() {
        _shouldBeConnected = false; // Stop autoreconnect
        
        if (WiFi.getMode() == WIFI_OFF) { return; }
        _notifyStable(false);
        
        // Stop config portal if active
        if (_wm.getConfigPortalActive()) {
            _wm.stopConfigPortal();
        }
        stopWebPortal();

        WiFi.disconnect(true); // true = turn off radio
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        
        // Change status immediately
        _checkStatus(millis());
    }

    void startWebPortal() {
        if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) {
            _fireEvent(EVENT_WARN_PORTAL);
            return;
        }

        if (!_wm.getWebPortalActive()) {
            _wm.startWebPortal(); 
            _fireEvent(EVENT_NOTICE_PORTAL);
        }
    }

    void stopWebPortal() {
        if (_wm.getWebPortalActive()) {
            _wm.stopWebPortal();
            _fireEvent(EVENT_NOTICE_PORTAL);
        }
        
    }

    namespace {
        // Reconnects or switches to AP
        static void _reconnect(uint32_t time) {
            _lastReconnectAttempt = time;
    
            if (_canFallbackToAp()) {
                _fireEvent(EVENT_WARN_MODE);
                startAP();
            } else {
                _fireEvent(EVENT_NOTICE_RECONNECT);
                WiFi.mode(WIFI_STA);
                _beginStaConnect();
            }
        }
    }

    Status loop() {
        if (!_initOk) setup();
        
        _wm.process();

        if (_otaUpdateStarted) {
            // Check error, stop GW check while OTA update is in progress
            // - early return current status to be as quick as possible
            if (!_otaUpdateFailed && Update.hasError()) {
                // OTA update failed event
                _otaUpdateFailed = true;
                _otaUpdateStarted = false;
                _fireEvent(EVENT_WARN_OTA_FAILED);
            }
            _stopGwCheck();
            return _currentStatus;
        }

        uint32_t now = millis();

        _loopGwCheck();

        // Auto reconnect logic
        if (_shouldBeConnected && WiFi.status() != WL_CONNECTED && !_wm.getConfigPortalActive()) {
            if (now - _lastReconnectAttempt >= _reconnectInterval) {
                _reconnect(now);
            }
        }

        if (now - _lastStatusCheck >= _statusCheckInterval) {
            _checkStatus(now);
        }

        return _currentStatus;
    }

    bool otaUpdateStarted() {
        return _otaUpdateStarted;
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

}