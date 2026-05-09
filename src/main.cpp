/**
 * SlimHuys P1-bridge firmware
 * --------------------------
 * Leest DSMR-telegrammen via UART + telt water-pulses op GPIO32, pusht
 * beide naar SlimHuys' /v1/me/readings + /v1/me/water-readings over
 * ethernet (LAN8720 + PoE) of WiFi-fallback.
 *
 * Pin-mapping (smarthomeshop WaterP1MeterKit V3):
 *   GPIO 16 ← P1-data (software-inverted UART, geen NPN nodig)
 *   GPIO 12 → P1-request (data-trigger naar slimme meter)
 *   GPIO 13 → groene LED van de RGB-indicator (ETH-status)
 *   GPIO 32 ← water-pulse (reed-switch, 1 puls = 1L default)
 *   LAN8720 PHY → ethernet RJ45 (MDC=23, MDIO=18, CLK=17_OUT, PWR=33, addr=1)
 *
 * Provisioning: bij eerste boot start een captive-portal op SSID
 * "SlimHuys-Setup-XXXX". User kiest WiFi + vult de 6-cijferige
 * pairing-code in (uit SlimHuys-app, MijnHuis → P1-bridge koppelen).
 * Device wisselt code in voor api-key via /v1/bridges/claim en bewaart
 * key + base_url in NVS.
 */
#include <Arduino.h>
#include <ESPmDNS.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <dsmr.h>
#include <driver/uart.h>
#include <time.h>

#include "management.h"
#include "portal.h"
#include "updater.h"

using namespace dsmr::fields;

// ============================================================================
// Configuration
// ============================================================================
constexpr int P1_RX_PIN = 16;       // DSMR-data (software-inverted UART)
constexpr int P1_REQUEST_PIN = 12;  // Data-trigger naar slimme meter (DTR)
constexpr int LED_PIN = 13;         // Groene kanaal van de RGB-indicator
constexpr int WATER_PULSE_PIN = 32; // Reed-switch op water-meter
constexpr int PUSH_INTERVAL_MS = 1000;             // P1: 1Hz
constexpr int WATER_PUSH_THROTTLE_MS = 1000;       // Water: max 1Hz bij verandering
constexpr int WATER_PUSH_HEARTBEAT_MS = 60000;     // Water: periodiek ook bij stilstand
constexpr unsigned long WATER_PERSIST_INTERVAL_MS = 5UL * 60UL * 1000UL; // NVS-write throttle
constexpr unsigned long WATER_DEBOUNCE_US = 50000; // 50ms reed-switch debounce
#ifndef LITERS_PER_PULSE
#define LITERS_PER_PULSE 1
#endif

// LAN8720 PHY — esp32dev heeft geen ETH_PHY_*-macro's, dus expliciete args
// aan ETH.begin() (zie setup()). Pin-keuze komt uit smarthomeshop V3 eth.yaml.

// ============================================================================
// State
// ============================================================================
Preferences prefs;
HardwareSerial P1Serial(2);
ManagementInterface management;
OtaUpdater updater;
String apiKey;
String baseUrl;
String deviceHostname;
volatile bool ethConnected = false;
unsigned long lastPushAt = 0;
unsigned long lastWaterPushAt = 0;
unsigned long lastWaterPersistAt = 0;
// Sentinel UINT32_MAX zodat de eerste vergelijking altijd "changed" zegt
// en de eerste push dus direct vuurt op boot.
uint32_t lastPushedTotal = 0xFFFFFFFFu;
uint32_t lastPersistedTotal = 0;

// Diagnostiek: hoeveel bytes hebben we überhaupt op de UART zien
// langskomen (telt vóór reader 'r consumeert). Bridge → UART-pin → level
// converter chain debuggen.
uint32_t p1BytesObserved = 0;

// Water-pulse counter (cumulatief sinds NVS-init). Geschreven door ISR,
// gelezen door main-loop. uint32_t is single-instruction load/store op
// xtensa, dus geen explicit lock nodig.
volatile uint32_t waterPulseCount = 0;
volatile unsigned long lastPulseUs = 0;

void IRAM_ATTR onWaterPulse() {
    unsigned long now = micros();
    // Reed-switches bouncen ~10-30ms — we negeren transitions binnen 50ms.
    if (now - lastPulseUs < WATER_DEBOUNCE_US) return;
    lastPulseUs = now;
    waterPulseCount++;
}

// Bouwt een uniek hostname zoals "slimhuys-p1-dd4240" — laatste 3 MAC-bytes
// als suffix zodat meerdere bridges op hetzelfde netwerk niet botsen.
String makeHostname() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "slimhuys-p1-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return String(buf);
}

// ============================================================================
// DSMR-velden — zelfde shape als HACS-integration v0.4.0 + backend
// ============================================================================
using P1Data = ParsedData<
    /* totalen tariff 1+2  */ energy_delivered_tariff1,
                              energy_delivered_tariff2,
                              energy_returned_tariff1,
                              energy_returned_tariff2,
    /* actief vermogen     */ power_delivered,
                              power_returned,
    /* currents per fase   */ current_l1, current_l2, current_l3,
    /* voltages per fase   */ voltage_l1, voltage_l2, voltage_l3,
    /* power per fase      */ power_delivered_l1, power_delivered_l2, power_delivered_l3,
                              power_returned_l1,  power_returned_l2,  power_returned_l3,
    /* gas                 */ gas_delivered>;

P1Reader reader(&P1Serial, P1_REQUEST_PIN);

// ============================================================================
// Helpers
// ============================================================================
void onNetworkEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            // Hostname zetten zodra de STA-interface up is maar nog niet
            // verbonden — Arduino-ESP32 silent-fails soms als setHostname
            // te vroeg in setup() wordt aangeroepen.
            if (!deviceHostname.isEmpty()) {
                WiFi.setHostname(deviceHostname.c_str());
            }
            break;
        case ARDUINO_EVENT_ETH_START:
            // Idem voor ETH — móet vóór DHCP, anders "espressif".
            if (!deviceHostname.isEmpty()) {
                ETH.setHostname(deviceHostname.c_str());
            }
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("ETH cable connected");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.print("ETH IP: ");
            Serial.println(ETH.localIP());
            ethConnected = true;
            digitalWrite(LED_PIN, HIGH);
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethConnected = false;
            digitalWrite(LED_PIN, LOW);
            break;
        default:
            break;
    }
}

bool networkReady() {
    return ethConnected || WiFi.status() == WL_CONNECTED;
}

String iso8601Now() {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    String s(buf);
    // %z geeft "+0200" (RFC822); SlimHuys-validator wil "+02:00" (ISO 8601
    // strict) — colon in de TZ-suffix schuiven.
    int len = s.length();
    if (len >= 5 && (s[len - 5] == '+' || s[len - 5] == '-')) {
        s = s.substring(0, len - 2) + ":" + s.substring(len - 2);
    }
    return s;
}

// Lees relevante velden voor live-display in de management-UI uit
// een geparsd telegram. Velden die niet aanwezig zijn blijven 0 — fine
// voor een dashboard, niet voor de API (die filtert op aanwezigheid).
LastReading toLastReading(P1Data& d) {
    LastReading r;
    r.valid = true;
    r.at_ms = millis();

    if (d.power_delivered_present) {
        int consumed = (int)(d.power_delivered.val() * 1000);
        int returned = d.power_returned_present ? (int)(d.power_returned.val() * 1000) : 0;
        r.active_power_w = consumed - returned;
        r.active_power_returned_w = returned;
    }
    if (d.voltage_l1_present) r.voltage_l1 = d.voltage_l1.val();
    if (d.voltage_l2_present) r.voltage_l2 = d.voltage_l2.val();
    if (d.voltage_l3_present) r.voltage_l3 = d.voltage_l3.val();
    if (d.current_l1_present) r.current_l1_a = d.current_l1;
    if (d.current_l2_present) r.current_l2_a = d.current_l2;
    if (d.current_l3_present) r.current_l3_a = d.current_l3;
    if (d.power_delivered_l1_present) r.active_power_l1_w = (int)(d.power_delivered_l1.val() * 1000);
    if (d.power_delivered_l2_present) r.active_power_l2_w = (int)(d.power_delivered_l2.val() * 1000);
    if (d.power_delivered_l3_present) r.active_power_l3_w = (int)(d.power_delivered_l3.val() * 1000);
    if (d.energy_delivered_tariff1_present && d.energy_delivered_tariff2_present) {
        r.consumption_kwh = d.energy_delivered_tariff1.val() + d.energy_delivered_tariff2.val();
    }
    if (d.energy_returned_tariff1_present && d.energy_returned_tariff2_present) {
        r.delivered_kwh = d.energy_returned_tariff1.val() + d.energy_returned_tariff2.val();
    }
    if (d.gas_delivered_present) r.gas_m3 = d.gas_delivered.val();
    r.water_l_total = waterPulseCount * (uint32_t)LITERS_PER_PULSE;
    return r;
}

// Genereer of laad het admin-password (12 chars, geen verwarrende tekens)
String getOrCreateAdminPassword() {
    String pw = prefs.getString("admin_pass", "");
    if (!pw.isEmpty()) return pw;

    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    pw = "";
    for (int i = 0; i < 12; i++) {
        pw += charset[esp_random() % (sizeof(charset) - 1)];
    }
    prefs.putString("admin_pass", pw);
    return pw;
}

// ============================================================================
// Push naar SlimHuys-API
// ============================================================================
void pushReading(P1Data& d) {
    if (!networkReady() || apiKey.isEmpty() || baseUrl.isEmpty()) return;

    JsonDocument doc;
    auto readings = doc["readings"].to<JsonArray>();
    auto r = readings.add<JsonObject>();
    r["timestamp"] = iso8601Now();

    // De DEFINE_FIELD-macro genereert per veld een ${name}_present-bool naast
    // ${name} zelf — directer dan de visitor-pattern voor losse velden.
    if (d.energy_delivered_tariff1_present && d.energy_delivered_tariff2_present) {
        r["consumption_kwh_total"] = d.energy_delivered_tariff1.val() + d.energy_delivered_tariff2.val();
    }
    if (d.energy_returned_tariff1_present && d.energy_returned_tariff2_present) {
        r["delivered_kwh_total"] = d.energy_returned_tariff1.val() + d.energy_returned_tariff2.val();
    }

    // Actieve vermogen — DSMR levert in kW, backend wil signed integer W
    if (d.power_delivered_present) {
        int consumed_w = (int)(d.power_delivered.val() * 1000);
        int returned_w = d.power_returned_present ? (int)(d.power_returned.val() * 1000) : 0;
        r["active_power_w"] = consumed_w - returned_w;  // signed netto
        r["active_power_returned_w"] = returned_w;
    }

    // Per-fase voltage + current
    if (d.voltage_l1_present) r["voltage_l1"] = d.voltage_l1.val();
    if (d.voltage_l2_present) r["voltage_l2"] = d.voltage_l2.val();
    if (d.voltage_l3_present) r["voltage_l3"] = d.voltage_l3.val();
    // Currents zijn IntField (uint16_t in hele ampères) — geen .val() nodig.
    if (d.current_l1_present) r["current_l1_a"] = d.current_l1;
    if (d.current_l2_present) r["current_l2_a"] = d.current_l2;
    if (d.current_l3_present) r["current_l3_a"] = d.current_l3;

    // Per-fase power (consumed + returned, backend rekent signed)
    if (d.power_delivered_l1_present) r["active_power_l1_w"] = (int)(d.power_delivered_l1.val() * 1000);
    if (d.power_delivered_l2_present) r["active_power_l2_w"] = (int)(d.power_delivered_l2.val() * 1000);
    if (d.power_delivered_l3_present) r["active_power_l3_w"] = (int)(d.power_delivered_l3.val() * 1000);
    if (d.power_returned_l1_present) r["active_power_returned_l1_w"] = (int)(d.power_returned_l1.val() * 1000);
    if (d.power_returned_l2_present) r["active_power_returned_l2_w"] = (int)(d.power_returned_l2.val() * 1000);
    if (d.power_returned_l3_present) r["active_power_returned_l3_w"] = (int)(d.power_returned_l3.val() * 1000);

    // Gas
    if (d.gas_delivered_present) r["gas_total_m3"] = d.gas_delivered.val();

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    http.begin(baseUrl + "/v1/me/readings");
    http.addHeader("Authorization", "Bearer " + apiKey);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "slimhuys-p1/" FIRMWARE_VERSION);

    int code = http.POST(payload);
    if (code != 202) {
        Serial.printf("POST faalde: HTTP %d\n", code);
    }
    http.end();
    management.recordPush(code);

    // Eerste succesvolle push = signaal dat deze firmware werkt; voorkom
    // bootloader-rollback naar de vorige versie. No-op als geen pending OTA.
    if (code == 202 || code == 200) {
        updater.markValid();
    }
}

// ============================================================================
// Push naar SlimHuys-API — management-info (LAN-IP + admin-creds)
// ----------------------------------------------------------------------------
// Idempotent: 1× na pairing, daarna alleen wanneer IP veranderd t.o.v.
// laatst-uploaded waarde (DHCP-lease-rotation, kabelwissel, etc.).
// SPA gebruikt dit om de "Toon gegevens"-knop in Mijn Huis te vullen.
// ============================================================================
void pushManagementInfo() {
    if (!networkReady() || apiKey.isEmpty() || baseUrl.isEmpty()) return;

    String currentIp = ethConnected ? ETH.localIP().toString() : WiFi.localIP().toString();
    String lastIp = prefs.getString("mgmt_ip", "");
    if (currentIp == lastIp) return;  // niets veranderd, skip de roundtrip

    String adminPass = prefs.getString("admin_pass", "");
    if (adminPass.isEmpty()) return;  // defensief — getOrCreateAdminPassword()
                                      // had 'm allang aangemaakt

    JsonDocument doc;
    doc["host"] = currentIp;
    doc["username"] = "admin";
    doc["password"] = adminPass;

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    http.begin(baseUrl + "/v1/me/bridges/management");
    http.addHeader("Authorization", "Bearer " + apiKey);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "slimhuys-p1/" FIRMWARE_VERSION);

    int code = http.PUT(payload);
    http.end();

    Serial.printf("Management-info PUT: HTTP %d (host=%s)\n", code, currentIp.c_str());

    if (code == 204 || code == 200) {
        prefs.putString("mgmt_ip", currentIp);
    }
}

// ============================================================================
// Push naar SlimHuys-API — water (cumulatieve liter-stand)
// ----------------------------------------------------------------------------
// Aanroeper bepaalt total_l (gesnapshot vóór de call zodat ISR-races niet
// tussen decision en payload kunnen optreden). Op success update lastPushedTotal
// in RAM — NVS-persist gebeurt los, op een eigen ~5min-cadens.
// ============================================================================
void pushWaterReading(uint32_t total_l) {
    if (!networkReady() || apiKey.isEmpty() || baseUrl.isEmpty()) return;

    JsonDocument doc;
    auto readings = doc["readings"].to<JsonArray>();
    auto r = readings.add<JsonObject>();
    r["timestamp"] = iso8601Now();
    r["total_liter"] = total_l;

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    http.begin(baseUrl + "/v1/me/water-readings");
    http.addHeader("Authorization", "Bearer " + apiKey);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "slimhuys-p1/" FIRMWARE_VERSION);

    int code = http.POST(payload);
    if (code != 202 && code != 200) {
        Serial.printf("Water-POST faalde: HTTP %d\n", code);
    }
    http.end();
    management.recordWaterPush(code);

    if (code == 202 || code == 200) {
        lastPushedTotal = total_l;
    }
}

// ============================================================================
// Setup + loop
// ============================================================================
void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    Serial.begin(115200);
    Serial.println("\n=== SlimHuys P1-bridge v" FIRMWARE_VERSION " ===");

    // Config laden uit NVS
    prefs.begin("slimhuys", false);
    apiKey = prefs.getString("api_key", "");
    baseUrl = prefs.getString("base_url", SLIMHUYS_BASE_URL);
    waterPulseCount = prefs.getUInt("water_l", 0);
    lastPersistedTotal = waterPulseCount;  // start in sync met NVS
    Serial.printf("Water-counter geladen: %u L\n", (unsigned)waterPulseCount);

    // Pulse-input: pull-up + falling edge ISR. Doen we vroeg in setup() zodat
    // pulses tijdens captive-portal of NTP-sync niet verloren gaan.
    pinMode(WATER_PULSE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(WATER_PULSE_PIN), onWaterPulse, FALLING);

    // Hostname opbouwen vóór WiFi/ETH starten — anders kondigt 't bord
    // zich aan als "esp32-XXXXXX" / "espressif" in de DHCP-tabel.
    deviceHostname = makeHostname();
    Serial.printf("Hostname: %s\n", deviceHostname.c_str());

    // Network event-listener (set ETH hostname op ETH_START)
    WiFi.onEvent(onNetworkEvent);

    // STA-mode initialiseren + hostname zetten — vóór WiFi.begin() (saved
    // creds én later in het portal). Hostname blijft sticky over begin/disconnect.
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(deviceHostname.c_str());

    // Modem-sleep uit: dit is een wired-power device, dus de ~30mA
    // besparing weegt niet op tegen de WiFi-stabiliteit (voorkomt random
    // NOT_AUTHED-disconnects op DTIM-grenzen).
    WiFi.setSleep(false);

    // Probeer opgeslagen WiFi-creds (parallel aan ethernet — wat 't eerst
    // up is, wordt gebruikt). Bij eerste boot zijn deze leeg → no-op.
    String savedSsid = prefs.getString("wifi_ssid", "");
    String savedPass = prefs.getString("wifi_pass", "");
    if (!savedSsid.isEmpty()) {
        Serial.printf("WiFi reconnect: %s\n", savedSsid.c_str());
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    }

    // Ethernet starten — kabel-detectie is async via onNetworkEvent.
    // Expliciete LAN8720-config (esp32dev-board kent geen ETH_PHY_*-defaults).
    Serial.println("ETH start…");
    ETH.begin(/*phy_addr*/ 1,
              /*power*/    33,
              /*mdc*/      23,
              /*mdio*/     18,
              /*type*/     ETH_PHY_LAN8720,
              /*clk_mode*/ ETH_CLOCK_GPIO17_OUT);

    // 10s wachten tot ETH óf WiFi up is
    unsigned long deadline = millis() + 10000;
    while (millis() < deadline && !networkReady()) {
        delay(100);
    }

    // Geen netwerk OF nog niet ge-paired? Start captive-portal voor setup.
    if (!networkReady() || apiKey.isEmpty()) {
        Serial.println("Captive portal start (geen netwerk of geen api-key)");
        CaptivePortal portal;
        String claimUrl = String(SLIMHUYS_BASE_URL) + "/v1/bridges/claim";
        // Ethernet-state doorgeven zodat het portaal alleen om de pairing-code
        // hoeft te vragen als de kabel al up is.
        if (!portal.run("SlimHuys-Setup", claimUrl, ethConnected)) {
            Serial.println("Portal timeout/error — reboot in 5s");
            delay(5000);
            ESP.restart();
        }

        apiKey = portal.apiKey();
        baseUrl = portal.baseUrl();
        prefs.putString("api_key", apiKey);
        prefs.putString("base_url", baseUrl);
        // WiFi-creds alleen bewaren als de gebruiker ze heeft ingevuld —
        // anders zou de ethernet-flow de eventuele bestaande creds wissen.
        if (!portal.ssid().isEmpty()) {
            prefs.putString("wifi_ssid", portal.ssid());
            prefs.putString("wifi_pass", portal.password());
        }
        Serial.println("Credentials bewaard in NVS");
    }

    // NTP voor ISO-timestamps
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org");

    // mDNS — bridge bereikbaar op http://<hostname>.local/
    if (MDNS.begin(deviceHostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local/\n", deviceHostname.c_str());
    }

    // Admin-password voor management-UI (basic auth op gevaarlijke endpoints)
    String adminPass = getOrCreateAdminPassword();

    // OTA-updater — periodiek check naar /v1/firmware/manifest
    updater.begin(baseUrl, apiKey, FIRMWARE_VERSION);

    // Lokale management-interface (status + reset-knoppen + OTA)
    management.begin(&prefs, FIRMWARE_VERSION, adminPass, &updater);
    Serial.printf("Management UI: http://%s/\n",
        ethConnected ? ETH.localIP().toString().c_str() : WiFi.localIP().toString().c_str());
    Serial.printf("  ↳ login: admin / %s\n", adminPass.c_str());

    // Upload host+creds naar SlimHuys zodat de SPA "Toon gegevens" kan tonen.
    // Idempotent — interne check skipt als IP onveranderd is t.o.v. NVS.
    pushManagementInfo();

    // P1-UART: 115200 8N1, RX-only (data komt naar ons toe).
    // Software-invert i.p.v. een NPN-inverter — moet ná begin() omdat
    // begin() de UART opnieuw configureert en de invert-flag overschrijft.
    // RX-buffer 2048B: DSMR 5.0-telegrammen kunnen ~900B groot zijn, default
    // 256B kan onder load overlopen en parses afbreken.
    P1Serial.setRxBufferSize(2048);
    P1Serial.begin(115200, SERIAL_8N1, P1_RX_PIN, -1);
    uart_set_line_inverse(UART_NUM_2, UART_SIGNAL_RXD_INV);
    reader.enable(true);

    Serial.println("Setup klaar. P1-data verwacht…");
}

void loop() {
    management.loop();
    updater.loop();

    // Water-push: change-driven met 1Hz throttle, plus 60s heartbeat. Geeft
    // snelle UI-feedback bij stromend water zonder push-storm bij heavy flow,
    // en zorgt dat backend ook bij stilstand periodiek bevestigd ziet "alive".
    unsigned long now = millis();
    uint32_t total_l = waterPulseCount * (uint32_t)LITERS_PER_PULSE;
    bool waterChanged = (total_l != lastPushedTotal);
    unsigned long sinceWaterPush = now - lastWaterPushAt;
    if ((waterChanged && sinceWaterPush >= WATER_PUSH_THROTTLE_MS)
            || sinceWaterPush >= WATER_PUSH_HEARTBEAT_MS) {
        pushWaterReading(total_l);
        lastWaterPushAt = now;
    }

    // NVS-persist los van push: max 1×/5min en alleen als waarde gewijzigd.
    // Beschermt flash bij heavy flow (bv. bad-vullen = 1 puls/sec ×10min).
    if (total_l != lastPersistedTotal && now - lastWaterPersistAt >= WATER_PERSIST_INTERVAL_MS) {
        prefs.putUInt("water_l", total_l);
        lastPersistedTotal = total_l;
        lastWaterPersistAt = now;
    }

    // Diagnostiek: tel bytes vóór reader 'r consumeert
    int avail = P1Serial.available();
    if (avail > 0) {
        p1BytesObserved += (uint32_t)avail;
    }

    reader.loop();

    if (reader.available()) {
        P1Data data;
        String error;
        if (reader.parse(&data, &error)) {
            management.recordParse(true);
            management.setLastReading(toLastReading(data));
            if (now - lastPushAt >= PUSH_INTERVAL_MS) {
                pushReading(data);
                lastPushAt = now;
            }
        } else {
            Serial.print("Parse-fout: ");
            Serial.println(error);
            management.recordParse(false, error);
        }
        reader.enable(true);  // klaar voor volgende telegram
    }

    delay(10);
}
