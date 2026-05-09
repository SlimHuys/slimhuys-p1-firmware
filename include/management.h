/**
 * SlimHuys management-interface
 * -----------------------------
 * Lokale web-UI voor status + beheer na pairing. Beschikbaar op poort 80
 * van het ETH/WiFi-IP en via mDNS op http://slimhuys-p1.local
 *
 * Public endpoints:
 *   GET  /                  → status-pagina (HTML)
 *   GET  /api/status        → JSON: netwerk, P1, laatste meting, tellers
 *
 * Auth-vereiste endpoints (HTTP Basic, user "admin", random pw uit NVS):
 *   POST /api/reboot        → herstart device
 *   POST /api/repair        → wis api_key, reboot → portal voor nieuwe code
 *   POST /api/factory-reset → wis alle NVS, reboot → eerste-setup
 *   POST /api/ota           → multipart firmware.bin upload, flash + reboot
 */
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>

#include "updater.h"

struct LastReading {
    bool valid = false;
    unsigned long at_ms = 0;

    int active_power_w = 0;          // signed netto (consumed - returned)
    int active_power_returned_w = 0;

    float voltage_l1 = 0, voltage_l2 = 0, voltage_l3 = 0;
    int current_l1_a = 0, current_l2_a = 0, current_l3_a = 0;
    int active_power_l1_w = 0, active_power_l2_w = 0, active_power_l3_w = 0;

    float consumption_kwh = 0;
    float delivered_kwh = 0;
    float gas_m3 = 0;
    uint32_t water_l_total = 0;       // cumulatief sinds NVS-init
};

class ManagementInterface {
public:
    void begin(Preferences* prefs, const String& version, const String& adminPassword,
               OtaUpdater* updater);
    void loop();

    // Telemetrie-hooks vanuit main-loop
    void recordPush(int httpStatus);
    void recordWaterPush(int httpStatus);
    void recordParse(bool ok, const String& error = String());
    void setLastReading(const LastReading& r) { _lastReading = r; }
    void setLeakDetected(bool detected) { _leakDetected = detected; }
    void setEnvReading(float temp_c, float humid_pct) {
        _envTempC = temp_c;
        _envHumidPct = humid_pct;
        _envValid = true;
    }

private:
    void _setupRoutes();
    bool _requireAuth();
    void _handleRoot();
    void _handleStatus();
    void _handleReboot();
    void _handleRepair();
    void _handleFactoryReset();
    void _handleOtaPost();
    void _handleOtaUpload();
    void _handleCheckUpdate();

    WebServer _server{80};
    Preferences* _prefs = nullptr;
    OtaUpdater* _updater = nullptr;
    String _version;
    String _adminUser = "admin";
    String _adminPass;

    int _lastPushStatus = 0;
    unsigned long _lastPushAt = 0;
    uint32_t _pushOk = 0;
    uint32_t _pushFail = 0;

    int _lastWaterPushStatus = 0;
    unsigned long _lastWaterPushAt = 0;
    uint32_t _waterPushOk = 0;
    uint32_t _waterPushFail = 0;

    bool _lastParseOk = true;
    String _lastParseError;
    unsigned long _lastParseAt = 0;
    uint32_t _parseOk = 0;
    uint32_t _parseFail = 0;
    // Consecutieve fails — UI toont pas rood na ≥3, voorkomt flikker bij
    // occasionele DSMR-glitches (1 telegram per 20s mist is normaal).
    uint32_t _consecutiveParseFails = 0;

    LastReading _lastReading;
    // Onafhankelijk van P1-state: leak-sensor moet ook werken zonder
    // DSMR-telegrammen, dus losse veld i.p.v. in LastReading.
    bool _leakDetected = false;
    // HDC1080 omgevings-readings (T+H), eveneens los van P1.
    bool _envValid = false;
    float _envTempC = 0;
    float _envHumidPct = 0;
};
