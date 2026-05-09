/**
 * SlimHuys captive portal
 * -----------------------
 * AP + DNS-hijack + single-page web-UI voor first-time setup.
 * Vraagt WiFi-creds + 6-cijferige pairing-code, wisselt code in voor
 * api-key via /v1/bridges/claim, en exposed creds aan main.cpp.
 *
 * Vervangt WiFiManager met een meer gebruiksvriendelijke flow:
 *   - Alle velden op één scherm (geen multi-step wizard)
 *   - Live status-feedback tijdens connect + claim
 *   - Mooie error-states ipv reboot-loops
 */
#pragma once
#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

class CaptivePortal {
public:
    /**
     * Start AP, host portal, blokkeer tot user klaar is of timeout verstrijkt.
     * @param ethReady  Als true: WiFi-velden verbergen; claim gaat direct over
     *                  ethernet. Als false: vraag WiFi-creds (oude flow).
     * @return true als pairing succesvol; false bij timeout.
     */
    bool run(const char* apSsid,
             const String& claimUrl,
             bool ethReady = false,
             unsigned long timeoutMs = 5UL * 60UL * 1000UL);

    String apiKey() const { return _apiKey; }
    String baseUrl() const { return _baseUrl; }
    String ssid() const { return _ssid; }
    String password() const { return _password; }

private:
    enum class State {
        IDLE,
        CONNECTING_WIFI,
        PAIRING,
        SUCCESS,
        ERROR_WIFI,
        ERROR_PAIRING,
    };

    void _setupRoutes();
    void _handleRoot();
    void _handleScan();
    void _handleSave();
    void _handleStatus();
    void _handleProbe();
    void _processBackground();
    bool _claim();

    WebServer _server{80};
    DNSServer _dns;
    State _state = State::IDLE;
    String _errorMsg;
    String _ssid;
    String _password;
    String _code;
    String _apiKey;
    String _baseUrl;
    String _claimUrl;
    bool _ethReady = false;
    unsigned long _stateChangedAt = 0;
    unsigned long _lastScanAt = 0;
};
