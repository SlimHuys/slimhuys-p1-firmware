/**
 * SlimHuys P1-bridge firmware
 * --------------------------
 * Leest DSMR-telegrammen via UART, pusht naar SlimHuys' /v1/me/readings
 * endpoint over ethernet (WT32-ETH01) of WiFi-fallback.
 *
 * Pin-mapping (WT32-ETH01):
 *   GPIO 5  ← P1-data via NPN-inverter (zie README)
 *   GPIO 33 → P1-request (data-trigger naar slimme meter)
 *   Built-in PHY → ethernet RJ45
 *
 * Provisioning: bij eerste boot start een captive-portal op SSID
 * "SlimHuys-Setup-XXXX". User kiest WiFi + vult de 6-cijferige
 * pairing-code in (uit SlimHuys-app, MijnHuis → P1-bridge koppelen).
 * Device wisselt code in voor api-key via /v1/bridges/claim en bewaart
 * key + base_url in NVS.
 */
#include <Arduino.h>
#include <ETH.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <dsmr.h>
#include <time.h>

// ============================================================================
// Configuration
// ============================================================================
constexpr int P1_RX_PIN = 5;        // DSMR-data uit NPN-inverter
constexpr int P1_REQUEST_PIN = 33;  // Data-trigger naar slimme meter (DTR)
constexpr int LED_PIN = 2;          // Status-LED (built-in op WT32-ETH01)
constexpr int PUSH_INTERVAL_MS = 1000;  // Min. tijd tussen pushes (1Hz)

// Ethernet config — WT32-ETH01 specifiek
constexpr int ETH_PHY_ADDR = 1;
constexpr int ETH_PHY_POWER = 16;
constexpr int ETH_PHY_MDC = 23;
constexpr int ETH_PHY_MDIO = 18;
constexpr eth_phy_type_t ETH_PHY = ETH_PHY_LAN8720;
constexpr eth_clock_mode_t ETH_CLK = ETH_CLOCK_GPIO0_IN;

// ============================================================================
// State
// ============================================================================
Preferences prefs;
HardwareSerial P1Serial(2);
String apiKey;
String baseUrl;
volatile bool ethConnected = false;
unsigned long lastPushAt = 0;

// ============================================================================
// DSMR-velden — zelfde shape als HACS-integration v0.4.0 + backend
// ============================================================================
using P1Data = ParsedData<
    /* energy_delivered_tariff1 */ EnergyDeliveredTariff1,
    /* energy_delivered_tariff2 */ EnergyDeliveredTariff2,
    /* energy_returned_tariff1  */ EnergyReturnedTariff1,
    /* energy_returned_tariff2  */ EnergyReturnedTariff2,
    /* power_delivered          */ PowerDelivered,
    /* power_returned           */ PowerReturned,
    /* electricity_currents     */ CurrentL1, CurrentL2, CurrentL3,
    /* electricity_voltages     */ VoltageL1, VoltageL2, VoltageL3,
    /* power_per_phase          */ PowerDeliveredL1, PowerDeliveredL2, PowerDeliveredL3,
                                   PowerReturnedL1,  PowerReturnedL2,  PowerReturnedL3,
    /* gas                      */ GasDelivered>;

P1Reader reader(&P1Serial, P1_REQUEST_PIN);

// ============================================================================
// Helpers
// ============================================================================
void onNetworkEvent(WiFiEvent_t event) {
    switch (event) {
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
    return String(buf);
}

// ============================================================================
// Pairing — wissel 6-cijferige code in voor api-key
// ============================================================================
bool claimPairingCode(const String& code, const String& claimUrl) {
    HTTPClient http;
    http.begin(claimUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "slimhuys-p1/" FIRMWARE_VERSION);

    JsonDocument req;
    req["code"] = code;
    String reqBody;
    serializeJson(req, reqBody);

    int status = http.POST(reqBody);
    String respBody = http.getString();
    http.end();

    if (status != 200) {
        Serial.printf("Claim faalde: HTTP %d — %s\n", status, respBody.c_str());
        return false;
    }

    JsonDocument resp;
    DeserializationError err = deserializeJson(resp, respBody);
    if (err) {
        Serial.println("Claim-response parse-fout");
        return false;
    }

    apiKey = resp["api_key"].as<String>();
    baseUrl = resp["base_url"].as<String>();
    if (apiKey.isEmpty() || baseUrl.isEmpty()) {
        Serial.println("Claim-response mist api_key of base_url");
        return false;
    }

    prefs.putString("api_key", apiKey);
    prefs.putString("base_url", baseUrl);
    Serial.println("Pairing succesvol — credentials bewaard");
    return true;
}

// ============================================================================
// Push naar SlimHuys-API
// ============================================================================
void pushReading(const P1Data& d) {
    if (!networkReady() || apiKey.isEmpty() || baseUrl.isEmpty()) return;

    JsonDocument doc;
    auto readings = doc["readings"].to<JsonArray>();
    auto r = readings.add<JsonObject>();
    r["timestamp"] = iso8601Now();

    // Cumulatieve totalen — som tariff 1 + 2
    if (d.energy_delivered_tariff1.present() && d.energy_delivered_tariff2.present()) {
        r["consumption_kwh_total"] = d.energy_delivered_tariff1.val() + d.energy_delivered_tariff2.val();
    }
    if (d.energy_returned_tariff1.present() && d.energy_returned_tariff2.present()) {
        r["delivered_kwh_total"] = d.energy_returned_tariff1.val() + d.energy_returned_tariff2.val();
    }

    // Actieve vermogen — DSMR levert in kW, backend wil signed integer W
    if (d.power_delivered.present()) {
        int consumed_w = (int)(d.power_delivered.val() * 1000);
        int returned_w = d.power_returned.present() ? (int)(d.power_returned.val() * 1000) : 0;
        r["active_power_w"] = consumed_w - returned_w;  // signed netto
        r["active_power_returned_w"] = returned_w;
    }

    // Per-fase voltage + current
    if (d.voltage_l1.present()) r["voltage_l1"] = d.voltage_l1.val();
    if (d.voltage_l2.present()) r["voltage_l2"] = d.voltage_l2.val();
    if (d.voltage_l3.present()) r["voltage_l3"] = d.voltage_l3.val();
    if (d.current_l1.present()) r["current_l1_a"] = d.current_l1.val();
    if (d.current_l2.present()) r["current_l2_a"] = d.current_l2.val();
    if (d.current_l3.present()) r["current_l3_a"] = d.current_l3.val();

    // Per-fase power (consumed + returned, backend rekent signed)
    if (d.power_delivered_l1.present()) r["active_power_l1_w"] = (int)(d.power_delivered_l1.val() * 1000);
    if (d.power_delivered_l2.present()) r["active_power_l2_w"] = (int)(d.power_delivered_l2.val() * 1000);
    if (d.power_delivered_l3.present()) r["active_power_l3_w"] = (int)(d.power_delivered_l3.val() * 1000);
    if (d.power_returned_l1.present()) r["active_power_returned_l1_w"] = (int)(d.power_returned_l1.val() * 1000);
    if (d.power_returned_l2.present()) r["active_power_returned_l2_w"] = (int)(d.power_returned_l2.val() * 1000);
    if (d.power_returned_l3.present()) r["active_power_returned_l3_w"] = (int)(d.power_returned_l3.val() * 1000);

    // Gas
    if (d.gas_delivered.present()) r["gas_total_m3"] = d.gas_delivered.val();

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

    // Network event-listener
    WiFi.onEvent(onNetworkEvent);

    // Ethernet-eerst, WiFi-fallback
    Serial.println("ETH start…");
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY, ETH_CLK);

    // 5s wachten op ethernet, anders WiFi-portal
    unsigned long ethDeadline = millis() + 5000;
    while (!ethConnected && millis() < ethDeadline) {
        delay(100);
    }

    // Geen ethernet OF nog niet ge-paired? Start captive-portal voor setup.
    // Pairing-code-veld + WiFi-creds; api-key wordt na claim opgehaald.
    if (!ethConnected || apiKey.isEmpty()) {
        Serial.println("Setup-portal start (geen ETH of geen api-key)");
        WiFiManager wm;
        WiFiManagerParameter codeParam(
            "code",
            "Pairing-code (6 cijfers, uit SlimHuys → MijnHuis)",
            "",
            6,
            "pattern='[0-9]{6}' inputmode='numeric'"
        );
        wm.addParameter(&codeParam);
        wm.setConfigPortalTimeout(300);

        // autoConnect blokkeert tot WiFi werkt of timeout. Bij geen opgeslagen
        // creds opent 'ie automatisch het portal-AP "SlimHuys-Setup".
        if (!wm.autoConnect("SlimHuys-Setup")) {
            Serial.println("Portal timeout — reboot in 10s");
            delay(10000);
            ESP.restart();
        }

        // WiFi werkt — als er een code is ingevuld én we zijn nog niet
        // gepaired, claim 'm.
        String code = String(codeParam.getValue());
        if (apiKey.isEmpty() && code.length() == 6) {
            String claimUrl = String(SLIMHUYS_BASE_URL) + "/v1/bridges/claim";
            if (!claimPairingCode(code, claimUrl)) {
                Serial.println("Pairing faalde — reboot om opnieuw te proberen");
                delay(5000);
                ESP.restart();
            }
        }

        if (apiKey.isEmpty()) {
            Serial.println("Geen api-key na portal — reboot");
            delay(5000);
            ESP.restart();
        }
    }

    // NTP voor ISO-timestamps
    configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org");

    // P1-UART: 115200 8N1, RX-only (data komt naar ons toe)
    P1Serial.begin(115200, SERIAL_8N1, P1_RX_PIN, -1);
    reader.enable(true);

    Serial.println("Setup klaar. P1-data verwacht…");
}

void loop() {
    reader.loop();

    if (reader.available()) {
        P1Data data;
        String error;
        if (reader.parse(&data, &error)) {
            unsigned long now = millis();
            if (now - lastPushAt >= PUSH_INTERVAL_MS) {
                pushReading(data);
                lastPushAt = now;
            }
        } else {
            Serial.print("Parse-fout: ");
            Serial.println(error);
        }
        reader.enable(true);  // klaar voor volgende telegram
    }

    delay(10);
}
