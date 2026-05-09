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
 *   GPIO 5  → RGB-indicator: rood kanaal (LEDC PWM)
 *   GPIO 13 → RGB-indicator: groen kanaal
 *   GPIO 14 → RGB-indicator: blauw kanaal
 *   GPIO 32 ← water-pulse (reed-switch, 1 puls = 1L default)
 *   GPIO 2  ← waterlek-sensor (active-low, INPUT_PULLUP)
 *   GPIO 15 ↔ I²C SDA (HDC1080 temp+vocht)
 *   GPIO 4  → I²C SCL
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
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <dsmr.h>
#include <driver/uart.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <functional>
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
constexpr int LED_R_PIN = 5;        // Rood kanaal RGB-indicator
constexpr int LED_G_PIN = 13;       // Groen kanaal
constexpr int LED_B_PIN = 14;       // Blauw kanaal
constexpr int LEDC_CH_R = 0;
constexpr int LEDC_CH_G = 1;
constexpr int LEDC_CH_B = 2;
constexpr int WATER_PULSE_PIN = 32; // Reed-switch op water-meter
constexpr int WATER_LEAK_PIN = 2;   // Lekkage-sensor (LOW=leak, INPUT_PULLUP)
constexpr int I2C_SDA_PIN = 15;     // HDC1080 temp+vocht
constexpr int I2C_SCL_PIN = 4;
constexpr uint8_t HDC1080_ADDR = 0x40;
constexpr unsigned long HDC_READ_INTERVAL_MS = 30000; // 1×/30s — ruim genoeg
// Self-heating compensatie — HDC1080 zit naast de ESP32 die warmte+droogte
// produceert. Offsets uit smarthomeshop's eigen ESPHome-config (klopt met
// onze hardware, geen reden om te tweaken).
constexpr float HDC_TEMP_OFFSET_C = -4.5f;
constexpr float HDC_HUMID_OFFSET_PCT = 12.0f;
// Asymmetrische leak-debounce: snel triggeren (100ms), traag herstellen (2s)
// zodat een opdrogend druppeltje het alarm niet uit-flickert.
constexpr unsigned long LEAK_ON_DEBOUNCE_MS = 100;
constexpr unsigned long LEAK_OFF_DEBOUNCE_MS = 2000;
// Sliding window voor live water-flow (L/min). 10s-buffer geeft snelle
// reactie zonder al te jittery rate.
constexpr int FLOW_WINDOW_S = 10;

// Watchdog + bootloop recovery
constexpr uint32_t WDT_TIMEOUT_S = 30;          // ruim boven HTTPClient TLS-handshake
constexpr uint32_t BOOTLOOP_THRESHOLD = 3;       // 3 crashes-in-een-rij → safe-mode
constexpr unsigned long BOOTLOOP_GRACE_MS = 60000; // na 60s uptime = "succesvol gebooten"

// Diagnostics-push: 1×/5min — vangt config-bugs en field-health zonder
// klantcontact. Backend throttle is 12/u, dus 5min is comfortabel binnen.
constexpr unsigned long DIAG_PUSH_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long DIAG_FIRST_PUSH_DELAY_MS = 30000; // 30s grace na boot
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
// en de eerste push dus direct vuurt op boot. volatile omdat de worker-
// task 'm schrijft in de push-callback en main-loop 'm leest.
volatile uint32_t lastPushedTotal = 0xFFFFFFFFu;
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

// Lek-sensor debounce: track laatste raw-read en wanneer 't omsloeg.
// Pas committen naar management als 't ≥LEAK_DEBOUNCE_MS stabiel is.
bool leakRawState = false;
bool leakStableState = false;
unsigned long leakChangedAt = 0;

// Lek-push state: pas POST'en als state-change verschilt van wat we de
// backend laatst hebben verteld. NVS-persist zodat een reboot middenin
// een leak niet opnieuw "detected"-spam veroorzaakt.
// volatile: schrijven gebeurt in worker-callback (core 0), main-loop leest.
volatile bool lastPushedLeakState = false;
volatile unsigned long leakNextRetryAt = 0;
volatile int leakRetryAttempts = 0;
// Exp. backoff schema voor 5xx/network: 15s, 60s, 5min, 30min cap.
constexpr unsigned long LEAK_BACKOFF_MS[] = {15000UL, 60000UL, 300000UL, 1800000UL};
constexpr unsigned long LEAK_4XX_COOLDOWN_MS = 1800000UL; // 30min na bridge-bug

// HDC1080 omgevings-readings — 1×/30s naar management gepusht
unsigned long lastHdcReadAt = 0;
bool hdcPresent = false;  // gezet bij eerste succesvolle init

// Live water-flow ring-buffer: één snapshot per seconde, oudste = "10s
// geleden". Pre-fill met de bootCount in setup() zodat warmup-fase
// 0 L/min toont i.p.v. een sprong vanuit 0.
uint32_t flowSamples[FLOW_WINDOW_S] = {0};
int flowSampleIdx = 0;
unsigned long lastFlowSampleAt = 0;
float currentFlowLpm = 0.0f;

// Cyan-flash op water-pulse — bevestigt visueel aan de gebruiker dat
// de meter live registreert. ISR signal'ert, loop voert de flash uit.
volatile bool waterPulseFlashRequested = false;
unsigned long waterPulseFlashUntil = 0;

// Bootloop-detectie. crash_count wordt bij elke boot ge-increment;
// na 60s succesvolle uptime reset we 'm. ≥3 wordt als "kapot" gezien
// → safe-mode (alleen management-UI, geen pushes/portal).
bool safeMode = false;
bool bootloopGraceCleared = false;

// Diagnostics-push state
unsigned long lastDiagPushAt = 0;
bool diagFirstPushDone = false;
uint32_t bootCount = 0;        // monotonic, niet gereset zoals crash_count
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;

// Persistente TLS-client voor HTTPClient. Eén instance hergebruiken
// betekent dat lwIP/mbedtls de TLS-sessie ID kan cachen — bij volgende
// requests slaat 't de zware public-key handshake over (~300ms → ~50ms).
// Wordt EXCLUSIEF door pushWorkerTask gebruikt (core 0), dus geen
// thread-safety zorgen.
WiFiClientSecure secureClient;

// HTTP-pushes draaien op een aparte FreeRTOS-task gepind aan core 0.
// De main-loop (core 1) blocked dus niet meer 50-400ms per TLS-roundtrip
// → WebServer reageert vlot, sensor-poll-cadens blijft 1Hz, geen
// gestaggerde pollintervals.
struct PushJob {
    String url;
    String method;       // "POST" of "PUT"
    String payload;
    std::function<void(int)> onComplete;
};

QueueHandle_t pushQueue = nullptr;
TaskHandle_t pushTaskHandle = nullptr;

// In-flight gate voor leak-push: voorkomt dat een tweede leak-event
// in de queue komt voordat de eerste callback de retry-state heeft
// bijgewerkt (race tussen state-change en callback).
volatile bool leakPushInFlight = false;

// LED-state-machine. LEDC PWM doet de blinks autonoom in hardware,
// dus de loop hoeft niks te toggle'en. Mode-transitions schrijven alleen
// nieuwe frequenties + duty naar de channels.
enum class LedMode {
    BOOT,           // groene continue (alles ok bij boot)
    PAIRING,        // blauwe pulse — captive-portal actief
    OPERATIONAL,    // groene continue
    ERROR,          // rode continue — geen netwerk of repeated push fail
    LEAK_ALARM,     // rode snelle blink — overruled alle andere
};
LedMode currentLedMode = LedMode::BOOT;

void IRAM_ATTR onWaterPulse() {
    unsigned long now = micros();
    // Reed-switches bouncen ~10-30ms — we negeren transitions binnen 50ms.
    if (now - lastPulseUs < WATER_DEBOUNCE_US) return;
    lastPulseUs = now;
    waterPulseCount++;
    waterPulseFlashRequested = true;
}

// Forward-decl — pickLedMode() heeft 'm nodig vóórdat networkReady() definitief
// gedefinieerd is verderop. Idem voor enqueuePush dat door push-functies
// hogerop in de file gebruikt wordt.
bool networkReady();
bool enqueuePush(const String& url, const String& method, const String& payload,
                 std::function<void(int)> onComplete);

// ============================================================================
// LED-state machine
// ----------------------------------------------------------------------------
// LEDC werkt op een channel-per-pin pattern: één-keer ledcAttachPin in
// initLeds(), daarna elke mode-transition gewoon ledcSetup (frequency)
// + ledcWrite (duty). Hardware doet de blink, geen Ticker nodig.
// ============================================================================
static void ledWrite(int channel, uint32_t freq_hz, uint8_t duty) {
    ledcSetup(channel, freq_hz, 8);
    ledcWrite(channel, duty);
}

void setLedMode(LedMode mode) {
    if (mode == currentLedMode) return;
    currentLedMode = mode;

    // Reset alle kanalen naar uit
    ledWrite(LEDC_CH_R, 5000, 0);
    ledWrite(LEDC_CH_G, 5000, 0);
    ledWrite(LEDC_CH_B, 5000, 0);

    switch (mode) {
        case LedMode::BOOT:
        case LedMode::OPERATIONAL:
            ledWrite(LEDC_CH_G, 5000, 255);  // groen continu
            break;
        case LedMode::PAIRING:
            ledWrite(LEDC_CH_B, 1, 128);     // 1Hz pulse blauw
            break;
        case LedMode::ERROR:
            ledWrite(LEDC_CH_R, 5000, 255);  // rood continu
            break;
        case LedMode::LEAK_ALARM:
            ledWrite(LEDC_CH_R, 4, 128);     // 4Hz blink rood
            break;
    }
}

void initLeds() {
    ledcAttachPin(LED_R_PIN, LEDC_CH_R);
    ledcAttachPin(LED_G_PIN, LEDC_CH_G);
    ledcAttachPin(LED_B_PIN, LEDC_CH_B);
    setLedMode(LedMode::BOOT);
}

// Bepaal de gewenste mode op basis van runtime-state. Wordt periodiek
// in loop() aangeroepen. Pairing wordt expliciet gezet rond portal.run().
LedMode pickLedMode() {
    if (leakStableState) return LedMode::LEAK_ALARM;
    if (!networkReady()) return LedMode::ERROR;
    return LedMode::OPERATIONAL;
}

// ============================================================================
// HDC1080 — temperatuur + vocht via I²C
// ============================================================================
bool hdc1080Init() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);

    // CONFIG register (0x02): MODE=1 (T then RH sequentieel), 14-bit elk,
    // heater off. Eerste byte 0x10, tweede 0x00.
    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(0x02);
    Wire.write(0x10);
    Wire.write(0x00);
    return Wire.endTransmission() == 0;
}

bool hdc1080Read(float& temp_c, float& humid_pct) {
    // Trigger meting door pointer-register naar 0x00 te schrijven
    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;

    // 14-bit T + 14-bit RH ≈ 13ms acquisition
    delay(15);

    Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)4);
    if (Wire.available() < 4) return false;
    uint16_t tRaw = ((uint16_t)Wire.read() << 8) | Wire.read();
    uint16_t hRaw = ((uint16_t)Wire.read() << 8) | Wire.read();

    temp_c = ((float)tRaw / 65536.0f) * 165.0f - 40.0f + HDC_TEMP_OFFSET_C;
    humid_pct = ((float)hRaw / 65536.0f) * 100.0f + HDC_HUMID_OFFSET_PCT;
    // Klamp humid-pct in geval calibratie 'm boven 100% of onder 0% duwt
    // bij extremen (zelden, maar kan gebeuren bij self-heating boundary).
    if (humid_pct > 100.0f) humid_pct = 100.0f;
    if (humid_pct < 0.0f) humid_pct = 0.0f;
    return true;
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
            // LED-mode wordt door de loop's pickLedMode() opgepakt; geen
            // directe digitalWrite hier nodig.
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethConnected = false;
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

    enqueuePush(baseUrl + "/v1/me/readings", "POST", payload, [](int code) {
        if (code != 202) {
            Serial.printf("POST faalde: HTTP %d\n", code);
        }
        management.recordPush(code);
        // Eerste succesvolle push = signaal dat deze firmware werkt; voorkom
        // bootloader-rollback naar de vorige versie. No-op als geen pending OTA.
        if (code == 202 || code == 200) {
            updater.markValid();
        }
    });
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
    doc["hostname"] = deviceHostname + ".local";  // mDNS-handle als alternatief op IP
    doc["username"] = "admin";
    doc["password"] = adminPass;

    String payload;
    serializeJson(doc, payload);

    enqueuePush(baseUrl + "/v1/me/bridges/management", "PUT", payload,
        [currentIp](int code) {
            Serial.printf("Management-info PUT: HTTP %d (host=%s)\n",
                          code, currentIp.c_str());
            if (code == 204 || code == 200) {
                prefs.putString("mgmt_ip", currentIp);
            }
        });
}

// ============================================================================
// Push-worker — alle HTTP-pushes draaien op core 0 (deze task), main-loop
// op core 1 enqueue't alleen. Zo blokkeert geen TLS-roundtrip ooit nog de
// WebServer of sensor-poll.
// ============================================================================
void pushWorkerTask(void* /*param*/) {
    esp_task_wdt_add(NULL);
    Serial.println("Push-worker gestart op core 0");
    for (;;) {
        esp_task_wdt_reset();
        PushJob* job = nullptr;
        if (xQueueReceive(pushQueue, &job, pdMS_TO_TICKS(5000)) != pdTRUE) {
            continue;  // periodieke wakeup voor WDT-reset
        }
        if (!job) continue;

        HTTPClient http;
        http.setReuse(true);
        http.begin(secureClient, job->url);
        http.addHeader("Authorization", "Bearer " + apiKey);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("User-Agent", "slimhuys-p1/" FIRMWARE_VERSION);

        int code = (job->method == "PUT")
                   ? http.PUT(job->payload)
                   : http.POST(job->payload);
        http.end();

        if (job->onComplete) job->onComplete(code);
        delete job;
    }
}

bool enqueuePush(const String& url, const String& method, const String& payload,
                 std::function<void(int)> onComplete) {
    if (!pushQueue) return false;
    if (!networkReady() || apiKey.isEmpty()) return false;

    PushJob* job = new PushJob{url, method, payload, std::move(onComplete)};
    if (xQueueSend(pushQueue, &job, 0) != pdTRUE) {
        Serial.println("Push-queue vol — request gedropt");
        delete job;
        return false;
    }
    return true;
}

void initPushWorker() {
    pushQueue = xQueueCreate(10, sizeof(PushJob*));
    xTaskCreatePinnedToCore(
        pushWorkerTask, "push-worker", 8192, nullptr,
        1 /* priority */, &pushTaskHandle, 0 /* core 0 */);
}

// ============================================================================
// Push naar SlimHuys-API — diagnostics (1×/5min field-health snapshot)
// ============================================================================
static const char* resetReasonString(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default:                return "UNKNOWN";
    }
}

void pushDiagnostics() {
    if (!networkReady() || apiKey.isEmpty() || baseUrl.isEmpty()) return;

    // Last-push-status afleiden van P1's meest recente HTTP-code. Backoff-
    // state tracken we alleen voor leak; voor de andere pushes is "fail" een
    // goede approximatie als de laatste roundtrip niet 200/202 was.
    int lastP1 = management.lastPushStatus();
    const char* statusStr = (lastP1 == 200 || lastP1 == 202) ? "ok"
                          : (lastP1 == 0)                    ? "ok"  // nog nooit gepusht
                          : "fail";

    JsonDocument doc;
    doc["boot_count"] = bootCount;
    doc["uptime_sec"] = millis() / 1000UL;
    doc["last_push_status"] = statusStr;
    doc["free_heap_bytes"] = ESP.getFreeHeap();
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_rssi_dbm"] = WiFi.RSSI();
    }
    doc["fw_version"] = FIRMWARE_VERSION;
    doc["reset_reason"] = resetReasonString(bootResetReason);

    String payload;
    serializeJson(doc, payload);

    enqueuePush(baseUrl + "/v1/me/bridges/diagnostics", "POST", payload,
        [](int code) {
            Serial.printf("Diag-POST: HTTP %d\n", code);
        });
}

// ============================================================================
// Push naar SlimHuys-API — lek-event (state-change only)
// ----------------------------------------------------------------------------
// Returnt HTTP-code, of -1 bij netwerk-/precondition-fail. Caller beslist
// wat 'r met de code gebeurt (zie loop-handler met backoff-policy).
// detected_at uit iso8601Now(); NTP-sync wachten zodat de alert-mail geen
// "1970"-timestamp krijgt.
// ============================================================================
void pushLeakState(bool detected) {
    if (leakPushInFlight) return;
    if (!networkReady() || apiKey.isEmpty() || baseUrl.isEmpty()) return;
    if (time(nullptr) < 1700000000) return;  // NTP nog niet gesynced

    JsonDocument doc;
    doc["detected"] = detected;
    doc["sensor_id"] = "main";
    doc["detected_at"] = iso8601Now();

    String payload;
    serializeJson(doc, payload);

    leakPushInFlight = true;
    bool ok = enqueuePush(baseUrl + "/v1/me/bridges/leak", "POST", payload,
        [detected](int code) {
            Serial.printf("Leak-POST (%s): HTTP %d\n", detected ? "WET" : "DRY", code);
            management.recordLeakPush(code);
            unsigned long now = millis();
            if (code == 204 || code == 200) {
                lastPushedLeakState = detected;
                prefs.putBool("leak_state", lastPushedLeakState);
                leakRetryAttempts = 0;
                leakNextRetryAt = 0;
            } else if (code >= 400 && code < 500) {
                Serial.printf("Leak-POST 4xx (%d) — geen retry, 30min cooldown\n", code);
                leakNextRetryAt = now + LEAK_4XX_COOLDOWN_MS;
            } else {
                int idx = leakRetryAttempts < 4 ? leakRetryAttempts : 3;
                leakNextRetryAt = now + LEAK_BACKOFF_MS[idx];
                leakRetryAttempts++;
                Serial.printf("Leak-POST retry-%d in %lums\n",
                              leakRetryAttempts, LEAK_BACKOFF_MS[idx]);
            }
            leakPushInFlight = false;
        });
    if (!ok) leakPushInFlight = false;  // enqueue faalde, gate weer vrij
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

    enqueuePush(baseUrl + "/v1/me/water-readings", "POST", payload,
        [total_l](int code) {
            if (code != 202 && code != 200) {
                Serial.printf("Water-POST faalde: HTTP %d\n", code);
            }
            management.recordWaterPush(code);
            if (code == 202 || code == 200) {
                lastPushedTotal = total_l;
            }
        });
}

// ============================================================================
// Setup + loop
// ============================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== SlimHuys P1-bridge v" FIRMWARE_VERSION " ===");
    bootResetReason = esp_reset_reason();
    Serial.printf("Reset reason: %d (%s)\n",
                  (int)bootResetReason, resetReasonString(bootResetReason));

    // RGB-indicator zo vroeg mogelijk: gebruiker ziet meteen of de bridge
    // boot. Groen = boot/operational, blauw-pulse = pairing, rood = error,
    // rood-blink = leak. Wordt verder door de loop bijgehouden.
    initLeds();

    // Bootloop-counter ophogen vóór risky init. Wordt op 0 gezet zodra
    // we 60s succesvol draaien. 3+ in een rij = ga in safe-mode zodat
    // de gebruiker via management-UI kan factory-resetten.
    Preferences bootPrefs;
    bootPrefs.begin("slimhuys", false);
    uint32_t crashCount = bootPrefs.getUInt("crash_count", 0) + 1;
    bootPrefs.putUInt("crash_count", crashCount);
    // boot_count is monotonic — telt elke boot (i.t.t. crash_count die op
    // 60s stabiele uptime gereset wordt). Naar diagnostics-push.
    bootCount = bootPrefs.getUInt("boot_count", 0) + 1;
    bootPrefs.putUInt("boot_count", bootCount);
    bootPrefs.end();
    Serial.printf("Boot #%u (crashes-in-een-rij: %u)\n",
                  (unsigned)bootCount, (unsigned)crashCount);
    if (crashCount >= BOOTLOOP_THRESHOLD) {
        safeMode = true;
        Serial.println("⚠️  SAFE-MODE — herhaalde crashes gedetecteerd");
        setLedMode(LedMode::ERROR);
    }

    // Persistente HTTPS-client zonder cert-pinning (slimhuys.nl is publieke
    // CA, een MITM op het lokale net is niet ons dreigingsmodel).
    secureClient.setInsecure();

    // Push-worker draait op core 0 — main-loop (core 1) blokkeert nu niet
    // meer 50-400ms per push. Initialiseren vóór de eerste enqueuePush.
    initPushWorker();

    // Config laden uit NVS
    prefs.begin("slimhuys", false);
    apiKey = prefs.getString("api_key", "");
    baseUrl = prefs.getString("base_url", SLIMHUYS_BASE_URL);
    waterPulseCount = prefs.getUInt("water_l", 0);
    lastPersistedTotal = waterPulseCount;  // start in sync met NVS
    lastPushedLeakState = prefs.getBool("leak_state", false);
    Serial.printf("Water-counter geladen: %u L; leak-state laatst gepusht: %s\n",
                  (unsigned)waterPulseCount, lastPushedLeakState ? "WET" : "DRY");

    // Flow-window pre-fillen: alle samples = current count → delta=0 →
    // flow=0 tot er echt iets stroomt. Voorkomt sprong vanuit 0 in de
    // eerste 10s na boot.
    for (int i = 0; i < FLOW_WINDOW_S; i++) flowSamples[i] = waterPulseCount;

    // Pulse-input: pull-up + falling edge ISR. Doen we vroeg in setup() zodat
    // pulses tijdens captive-portal of NTP-sync niet verloren gaan.
    pinMode(WATER_PULSE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(WATER_PULSE_PIN), onWaterPulse, FALLING);

    // Lek-sensor: active-low (sensor pulled-up via interne resistor, lek = LOW).
    // GPIO2 is een strapping pin maar runtime-gebruik als input is veilig.
    pinMode(WATER_LEAK_PIN, INPUT_PULLUP);

    // HDC1080 initialiseren — fail-soft: als 't fail't (bv. defect, niet
    // gemonteerd) draait alles verder gewoon zonder env-readings.
    hdcPresent = hdc1080Init();
    Serial.printf("HDC1080: %s\n", hdcPresent ? "ok" : "niet gevonden");

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

    // Safe-mode: skip captive-portal en remote-init. Management-UI komt
    // verderop wel up zodat de gebruiker factory-reset kan doen.
    if (!safeMode && (!networkReady() || apiKey.isEmpty())) {
        Serial.println("Captive portal start (geen netwerk of geen api-key)");
        setLedMode(LedMode::PAIRING);  // blauw pulserend tot pairing klaar is
        CaptivePortal portal;
        String claimUrl = String(SLIMHUYS_BASE_URL) + "/v1/bridges/claim";
        // Ethernet-state doorgeven zodat het portaal alleen om de pairing-code
        // hoeft te vragen als de kabel al up is.
        if (!portal.run("SlimHuys-Setup", claimUrl, ethConnected)) {
            Serial.println("Portal timeout/error — reboot in 5s");
            setLedMode(LedMode::ERROR);
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

    // OTA-updater — periodiek check naar /v1/firmware/manifest. Skip in
    // safe-mode: een kapotte firmware over OTA halen lost niks op.
    if (!safeMode) {
        updater.begin(baseUrl, apiKey, FIRMWARE_VERSION);
    }

    // Lokale management-interface (status + reset-knoppen + OTA) — altijd
    // up, ook in safe-mode (essentieel voor recovery via de gebruiker).
    management.begin(&prefs, FIRMWARE_VERSION, adminPass, &updater);
    management.setSafeMode(safeMode);
    Serial.printf("Management UI: http://%s/\n",
        ethConnected ? ETH.localIP().toString().c_str() : WiFi.localIP().toString().c_str());
    Serial.printf("  ↳ login: admin / %s\n", adminPass.c_str());

    // Upload host+creds naar SlimHuys zodat de SPA "Toon gegevens" kan tonen.
    // Idempotent — interne check skipt als IP onveranderd is t.o.v. NVS.
    if (!safeMode) {
        pushManagementInfo();
    }

    // P1-UART: 115200 8N1, RX-only (data komt naar ons toe).
    // Software-invert i.p.v. een NPN-inverter — moet ná begin() omdat
    // begin() de UART opnieuw configureert en de invert-flag overschrijft.
    // RX-buffer 2048B: DSMR 5.0-telegrammen kunnen ~900B groot zijn, default
    // 256B kan onder load overlopen en parses afbreken.
    if (!safeMode) {
        P1Serial.setRxBufferSize(2048);
        P1Serial.begin(115200, SERIAL_8N1, P1_RX_PIN, -1);
        uart_set_line_inverse(UART_NUM_2, UART_SIGNAL_RXD_INV);
        reader.enable(true);
    }

    // Watchdog activeren ná setup() — captive-portal en NTP-sync kunnen
    // tijdens setup() lang blocking zijn, wat WDT zou triggeren.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
    Serial.printf("Setup klaar (%s). %s\n",
                  safeMode ? "SAFE-MODE" : "normaal",
                  safeMode ? "Wachten op factory-reset via management-UI." : "P1-data verwacht…");
}

void loop() {
    esp_task_wdt_reset();

    // Bootloop-grace: 60s+ uptime = stabiel = crash_count terug naar 0.
    // Eenmalig per boot, daarna no-op.
    if (!bootloopGraceCleared && millis() >= BOOTLOOP_GRACE_MS) {
        prefs.putUInt("crash_count", 0);
        bootloopGraceCleared = true;
        Serial.println("Bootloop-counter gereset (60s+ uptime stabiel).");
    }

    management.loop();
    if (safeMode) {
        delay(10);
        return;  // skip alle pushes/parses tot factory-reset
    }
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

    // Lek-sensor: asymmetrische debounce — snel triggeren (100ms), traag
    // herstellen (2s) zodat een opdrogend kontact 't alarm niet uitflikkert.
    bool leakRaw = (digitalRead(WATER_LEAK_PIN) == LOW);
    if (leakRaw != leakRawState) {
        leakRawState = leakRaw;
        leakChangedAt = now;
    }
    if (leakRawState != leakStableState) {
        unsigned long required = leakRawState ? LEAK_ON_DEBOUNCE_MS : LEAK_OFF_DEBOUNCE_MS;
        if (now - leakChangedAt >= required) {
            leakStableState = leakRawState;
            Serial.printf("Lekkage-sensor: %s\n", leakStableState ? "GEDETECTEERD" : "geen");
            management.setLeakDetected(leakStableState);
            // State-change reset retry-state zodat een nieuwe transitie
            // direct probeert te pushen (oude backoff-timer geannuleerd).
            leakNextRetryAt = 0;
            leakRetryAttempts = 0;
        }
    }

    // Lek-push: alleen op state-change pushen. Async via worker-task —
    // retry-policy + NVS-write zit in de callback (zie pushLeakState).
    if (leakStableState != lastPushedLeakState && now >= leakNextRetryAt && !leakPushInFlight) {
        pushLeakState(leakStableState);
    }

    // HDC1080: 1×/30s temp+vocht binnenhalen en aan management doorgeven.
    if (hdcPresent && now - lastHdcReadAt >= HDC_READ_INTERVAL_MS) {
        float temp_c, humid_pct;
        if (hdc1080Read(temp_c, humid_pct)) {
            management.setEnvReading(temp_c, humid_pct);
        }
        lastHdcReadAt = now;
    }

    // Live water-flow sliding window. Sample 1×/sec; oudste in ring is
    // exact 10s geleden, dus delta = pulses-in-window. ×6 → L/min.
    if (now - lastFlowSampleAt >= 1000) {
        int oldestIdx = (flowSampleIdx + 1) % FLOW_WINDOW_S;
        uint32_t deltaPulses = waterPulseCount - flowSamples[oldestIdx];
        currentFlowLpm = (float)deltaPulses * (60.0f / FLOW_WINDOW_S);
        flowSamples[flowSampleIdx] = waterPulseCount;
        flowSampleIdx = (flowSampleIdx + 1) % FLOW_WINDOW_S;
        lastFlowSampleAt = now;
        management.setWaterFlow(currentFlowLpm);
    }

    // Diagnostics-push: 30s na boot (eerste keer met reset_reason van
    // de afgelopen reset), daarna elke 5min. Backend throttle is 12/u.
    if (!diagFirstPushDone && now >= DIAG_FIRST_PUSH_DELAY_MS) {
        pushDiagnostics();
        lastDiagPushAt = now;
        diagFirstPushDone = true;
    } else if (diagFirstPushDone && now - lastDiagPushAt >= DIAG_PUSH_INTERVAL_MS) {
        pushDiagnostics();
        lastDiagPushAt = now;
    }

    // LED-mode hertonen op basis van runtime-state. PAIRING wordt apart
    // gezet rond portal.run(); deze tak handelt LEAK/ERROR/OPERATIONAL.
    setLedMode(pickLedMode());

    // Cyan-flash op water-pulse — alleen in OPERATIONAL want andere modes
    // gebruiken al actief de blue/red kanalen.
    if (waterPulseFlashRequested) {
        waterPulseFlashRequested = false;
        if (currentLedMode == LedMode::OPERATIONAL) {
            ledcWrite(LEDC_CH_B, 80);   // moderate blue overlay → cyan met groen
            waterPulseFlashUntil = now + 50;
        }
    }
    if (waterPulseFlashUntil > 0 && now >= waterPulseFlashUntil) {
        waterPulseFlashUntil = 0;
        if (currentLedMode == LedMode::OPERATIONAL) {
            ledcWrite(LEDC_CH_B, 0);    // terug naar pure groen
        }
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
