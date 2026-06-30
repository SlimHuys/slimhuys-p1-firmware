/**
 * SlimHuys P1-bridge firmware
 * --------------------------
 * Leest DSMR-telegrammen via UART + telt water-pulses op GPIO32, pusht
 * beide naar SlimHuys' /v1/me/readings + /v1/me/water-readings over
 * ethernet (LAN8720 + PoE) of WiFi-fallback.
 *
 * Pin-mapping (smarthomeshop WaterP1MeterKit V3 — esp32dev, LAN8720 PoE):
 *   GPIO 16 ← P1-data (software-inverted UART2, geen NPN nodig)
 *   GPIO 12 → P1-request (data-trigger naar slimme meter)
 *   GPIO 5  → RGB-indicator: rood kanaal (LEDC PWM ch0)
 *   GPIO 13 → RGB-indicator: groen kanaal (ch1)
 *   GPIO 14 → RGB-indicator: blauw kanaal (ch2)
 *   GPIO 32 ← water-pulse (reed-switch, 1 puls = 1L default)
 *   GPIO 2  ← waterlek-sensor (active-low, INPUT_PULLUP)
 *   GPIO 15 ↔ I²C SDA (HDC1080 temp+vocht)
 *   GPIO 4  → I²C SCL
 *   LAN8720 PHY → ethernet RJ45 (MDC=23, MDIO=18, CLK=17_OUT, PWR=33, addr=1)
 *
 * Pin-mapping (smarthomeshop WaterP1MeterKit V4 — ESP32-C6, W5500 SPI):
 *   GPIO 1  ← P1-data (software-inverted UART1, geen NPN nodig)
 *   GPIO 10 → P1-request (data-trigger naar slimme meter)
 *   GPIO 13 → RGB-indicator: rood kanaal (LEDC PWM)
 *   GPIO 12 → RGB-indicator: groen kanaal
 *   GPIO 11 → RGB-indicator: blauw kanaal
 *   GPIO 0  ← water-pulse (reed-switch, 1 puls = 1L default)
 *   GPIO 5  ← waterlek/expansie-sensor (active-low, INPUT_PULLUP)
 *   GPIO 6  ↔ I²C SDA (HDC1080 temp+vocht)
 *   GPIO 7  → I²C SCL
 *   W5500 SPI-ethernet: PWR=18(act-low), SCK=23, MISO=22, MOSI=21, CS=15, INT=20, RST=19
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
#ifdef BOARD_V4
#include <SPI.h>
#include <esp_mac.h>       // esp_read_mac + ESP_MAC_WIFI_STA (in ESP-IDF 5.x apart van esp_system.h)
#include <esp_task_wdt.h>  // esp_task_wdt_config_t (andere signatuur in IDF 5.x)
#endif
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

#include "diaglog.h"
#include "management.h"
#include "portal.h"
#include "updater.h"

using namespace dsmr::fields;

// ============================================================================
// Configuration
// ============================================================================
#ifdef BOARD_V4
// smarthomeshop WaterP1MeterKit V4 — ESP32-C6 + W5500 SPI ethernet
constexpr int P1_RX_PIN = 1;
constexpr int P1_REQUEST_PIN = 10;
constexpr int LED_R_PIN = 13;
constexpr int LED_G_PIN = 12;
constexpr int LED_B_PIN = 11;
constexpr int WATER_PULSE_PIN = 0;
constexpr int WATER_LEAK_PIN = 5;
constexpr int I2C_SDA_PIN = 6;
constexpr int I2C_SCL_PIN = 7;
// W5500 SPI ethernet — power-enable actief-laag (GPIO18 LOW = aan)
constexpr int ETH_PWR_PIN  = 18;
constexpr int ETH_SCK_PIN  = 23;
constexpr int ETH_MISO_PIN = 22;
constexpr int ETH_MOSI_PIN = 21;
constexpr int ETH_CS_PIN   = 15;
constexpr int ETH_INT_PIN  = 20;
constexpr int ETH_RST_PIN  = 19;
// LEDC: V4 gebruikt de arduino-esp32 3.x pin-gebaseerde API — geen channelnummers
#define LED_R_ID LED_R_PIN
#define LED_G_ID LED_G_PIN
#define LED_B_ID LED_B_PIN
#else
// smarthomeshop WaterP1MeterKit V3 — ESP32 + LAN8720 RMII ethernet (PoE)
constexpr int P1_RX_PIN = 16;
constexpr int P1_REQUEST_PIN = 12;
constexpr int LED_R_PIN = 5;
constexpr int LED_G_PIN = 13;
constexpr int LED_B_PIN = 14;
constexpr int LEDC_CH_R = 0;
constexpr int LEDC_CH_G = 1;
constexpr int LEDC_CH_B = 2;
constexpr int WATER_PULSE_PIN = 32;
constexpr int WATER_LEAK_PIN = 2;
constexpr int I2C_SDA_PIN = 15;
constexpr int I2C_SCL_PIN = 4;
// LEDC: V3 gebruikt de arduino-esp32 2.x channel-gebaseerde API
#define LED_R_ID LEDC_CH_R
#define LED_G_ID LEDC_CH_G
#define LED_B_ID LEDC_CH_B
#endif
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
// Last-resort self-recovery: een bridge die in safe-mode hangt is onbereikbaar
// voor z'n eigenlijke taak. Na 4u in safe-mode 1× automatisch herstarten — de
// 60s-grace heeft crash_count dan al naar 0 gezet, dus de reboot boot normaal.
// Was 't een transiente oorzaak (brownout) → self-healed. Is 't een echte
// fault → 3 snelle crashes brengen 'm terug in safe-mode, dus dit blijft een
// zachte 4u-backoff i.p.v. een tight loop, met de UI-recovery-window intact.
constexpr unsigned long SAFEMODE_AUTO_REBOOT_MS = 4UL * 3600UL * 1000UL;

// Diagnostics-push: 1×/5min — vangt config-bugs en field-health zonder
// klantcontact. Backend throttle is 12/u, dus 5min is comfortabel binnen.
constexpr unsigned long DIAG_PUSH_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long DIAG_FIRST_PUSH_DELAY_MS = 30000; // 30s grace na boot

// Management-info re-push gate. Voorheen short-circuit'te pushManagementInfo()
// alleen op IP-equality, dus backend-state-drift (DB-restore, migration,
// row-loss) repareerde pas bij IP-change of reboot. Nu: re-push min 1×/24h
// sinds laatste success, plus on-demand zodra backend X-Mgmt-Refresh: 1 op
// een 2xx-response zet. Loop check't 1×/u (functie self-gate't), elk attempt
// throttled op max 1×/60s zodat een blijvend backend-signaal geen storm geeft.
constexpr uint32_t MGMT_REFRESH_INTERVAL_S = 24UL * 3600UL;
constexpr unsigned long MGMT_LOOP_CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL;
constexpr unsigned long MGMT_REFRESH_MIN_RETRY_MS = 60UL * 1000UL;
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
#ifdef BOARD_V4
HardwareSerial P1Serial(1);  // UART1 (ESP32-C6 heeft geen UART2)
#else
HardwareSerial P1Serial(2);  // UART2
#endif
ManagementInterface management;
OtaUpdater updater;
String apiKey;
String baseUrl;
String deviceHostname;
volatile bool ethConnected = false;
unsigned long ethLinkUpAt = 0;      // tijdstip waarop ETH-kabel verbonden werd
unsigned long ethDriverStartedAt = 0; // tijdstip waarop ETH-driver gestart is
volatile bool ethDhcpRefreshNeeded = false;  // vlag: loop moet DHCP herstarten
bool ethDhcpRetried = false;                 // éénmalige 30s-retry al gedaan?
bool ethPhyRetried = false;                  // éénmalige PHY-herstart al gedaan?
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

// Management-info push-state. Push-worker (core 0) zet refreshRequested op
// true zodra backend X-Mgmt-Refresh: 1 antwoordt; main-loop (core 1) consumeert
// 'm. lastMgmtCheckAt tracked beide loop-triggers (periodic + refresh) zodat
// ze één gedeelde 60s-throttle delen.
unsigned long lastMgmtCheckAt = 0;
volatile bool mgmtRefreshRequested = false;

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

// Opgeslagen WiFi-creds — nodig voor herverbinding in loop().
String wifiSsid;
String wifiPass;
unsigned long lastWifiReconnectAt = 0;

// Signaal vanuit netwerk-event naar push-worker: secureClient resetten
// na WiFi/ETH reconnect zodat de eerste post-reconnect push meteen een
// verse TLS-handshake doet i.p.v. 5s te wachten op de stale socket.
volatile bool forceSecureClientReset = false;

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
// V3 (arduino-esp32 2.x): id = LEDC channel-nummer; ledcSetup + ledcWrite.
// V4 (arduino-esp32 3.x): id = GPIO pin-nummer;    ledcChangeFrequency + ledcWrite.
static void ledWrite(int id, uint32_t freq_hz, uint8_t duty) {
#ifdef BOARD_V4
    ledcChangeFrequency(id, freq_hz, 8);
    ledcWrite(id, duty);
#else
    ledcSetup(id, freq_hz, 8);
    ledcWrite(id, duty);
#endif
}

void setLedMode(LedMode mode) {
    if (mode == currentLedMode) return;
    currentLedMode = mode;

    // Reset alle kanalen naar uit
    ledWrite(LED_R_ID, 5000, 0);
    ledWrite(LED_G_ID, 5000, 0);
    ledWrite(LED_B_ID, 5000, 0);

    switch (mode) {
        case LedMode::BOOT:
        case LedMode::OPERATIONAL:
            ledWrite(LED_G_ID, 5000, 255);  // groen continu
            break;
        case LedMode::PAIRING:
            ledWrite(LED_B_ID, 1, 128);     // 1Hz pulse blauw
            break;
        case LedMode::ERROR:
            ledWrite(LED_R_ID, 5000, 255);  // rood continu
            break;
        case LedMode::LEAK_ALARM:
            ledWrite(LED_R_ID, 4, 128);     // 4Hz blink rood
            break;
    }
}

void initLeds() {
#ifdef BOARD_V4
    // arduino-esp32 3.x: pin-gebaseerde attach (geen channelnummers)
    ledcAttach(LED_R_PIN, 5000, 8);
    ledcAttach(LED_G_PIN, 5000, 8);
    ledcAttach(LED_B_PIN, 5000, 8);
#else
    // arduino-esp32 2.x: channel-gebaseerde attach
    ledcAttachPin(LED_R_PIN, LEDC_CH_R);
    ledcAttachPin(LED_G_PIN, LEDC_CH_G);
    ledcAttachPin(LED_B_PIN, LEDC_CH_B);
#endif
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
    /* meter-timestamp     */ timestamp,
    /* meter-serienummer   */ equipment_id,
    /* totalen tariff 1+2  */ energy_delivered_tariff1,
                              energy_delivered_tariff2,
                              energy_returned_tariff1,
                              energy_returned_tariff2,
    /* actief tarief       */ electricity_tariff,
    /* actief vermogen     */ power_delivered,
                              power_returned,
    /* stroomstoringen     */ electricity_failures,
                              electricity_long_failures,
    /* currents per fase   */ current_l1, current_l2, current_l3,
    /* voltages per fase   */ voltage_l1, voltage_l2, voltage_l3,
    /* spanningskwaliteit  */ electricity_sags_l1,   electricity_sags_l2,   electricity_sags_l3,
                              electricity_swells_l1, electricity_swells_l2, electricity_swells_l3,
    /* power per fase      */ power_delivered_l1, power_delivered_l2, power_delivered_l3,
                              power_returned_l1,  power_returned_l2,  power_returned_l3,
    /* gas                 */ gas_delivered,
    /* gas-serienummer     */ gas_equipment_id>;

P1Reader reader(&P1Serial, P1_REQUEST_PIN);

// ============================================================================
// Helpers
// ============================================================================

void ethPhyInit() {
#ifdef BOARD_V4
    // V4: W5500 SPI ethernet. Power-enable actief-laag (GPIO18 LOW = aan).
    pinMode(ETH_PWR_PIN, OUTPUT);
    digitalWrite(ETH_PWR_PIN, LOW);
    delay(100);
    SPI.begin(ETH_SCK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, -1);
    diagLog("ETH.begin() (W5500 SPI cs=%d int=%d rst=%d)", ETH_CS_PIN, ETH_INT_PIN, ETH_RST_PIN);
    bool ok = ETH.begin(ETH_PHY_W5500, -1, ETH_CS_PIN, ETH_INT_PIN, ETH_RST_PIN, SPI);
    diagLog("ETH.begin() return: %s", ok ? "true" : "false");
#else
    // V3: LAN8720 RMII ethernet. Power-cycle NRST (GPIO33): 500ms laag, 500ms hoog.
    // Library-reset overgeslagen (power=-1) omdat we het handmatig doen.
    pinMode(33, OUTPUT);
    digitalWrite(33, LOW);
    delay(500);
    digitalWrite(33, HIGH);
    delay(500);
    diagLog("ETH.begin() (phy=1 pwr=33 mdc=23 mdio=18 LAN8720 clk17out)");
    bool ok = ETH.begin(/*phy_addr*/ 1, /*power*/ -1,
                        /*mdc*/ 23, /*mdio*/ 18,
                        ETH_PHY_LAN8720, ETH_CLOCK_GPIO17_OUT);
    diagLog("ETH.begin() return: %s", ok ? "true" : "false");
#endif
}

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
            diagLog("ETH: driver gestart");
            ethDriverStartedAt = millis();
            ethPhyRetried = false;
            if (!deviceHostname.isEmpty()) {
                ETH.setHostname(deviceHostname.c_str());
            }
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            diagLog("ETH: kabel verbonden");
            ethLinkUpAt = millis();
            ethDhcpRetried = false;
            ethDhcpRefreshNeeded = true;  // loop triggert DHCP-refresh
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            diagLog("ETH: IP %s", ETH.localIP().toString().c_str());
            ethConnected = true;
            forceSecureClientReset = true;
            // ETH heeft prioriteit: auto-reconnect uit + WiFi-sessie verbreken,
            // credentials bewaren zodat WiFi als fallback beschikbaar blijft.
            WiFi.setAutoReconnect(false);
            if (WiFi.status() == WL_CONNECTED) {
                diagLog("ETH up — WiFi-sessie verbroken (credentials bewaard)");
                WiFi.disconnect(false);
            }
            // LED-mode wordt door de loop's pickLedMode() opgepakt; geen
            // directe digitalWrite hier nodig.
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            diagLog("ETH: verbinding verloren");
            ethConnected = false;
            ethDhcpRetried = false;
            // ETH weg — auto-reconnect aan + WiFi als fallback starten.
            if (!wifiSsid.isEmpty()) {
                diagLog("ETH weg — WiFi-fallback starten");
                WiFi.setAutoReconnect(true);
                WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
            }
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            diagLog("WiFi: verbinding verloren");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            diagLog("WiFi: IP %s", WiFi.localIP().toString().c_str());
            forceSecureClientReset = true;
            break;
        default:
            break;
    }
}

bool networkReady() {
    return ethConnected || WiFi.status() == WL_CONNECTED;
}

// DSMR-timestamp "YYMMDDhhmmssX" (X=S zomer/W winter) → ISO 8601.
String dsmrToIso8601(const String& ts) {
    if (ts.length() < 13) return String();
    char buf[32];
    snprintf(buf, sizeof(buf), "20%c%c-%c%c-%c%cT%c%c:%c%c:%c%c%s",
             ts[0], ts[1], ts[2], ts[3], ts[4], ts[5],
             ts[6], ts[7], ts[8], ts[9], ts[10], ts[11],
             (ts[12] == 'S' || ts[12] == 's') ? "+02:00" : "+01:00");
    return String(buf);
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
    if (d.gas_delivered_present) {
        r["gas_total_m3"] = d.gas_delivered.val();
        String gasTs = dsmrToIso8601(d.gas_delivered.timestamp);
        if (gasTs.length()) r["gas_timestamp"] = gasTs;
    }

    // Meter-timestamp (uit het telegram zelf)
    if (d.timestamp_present) {
        String meterTs = dsmrToIso8601(d.timestamp);
        if (meterTs.length()) r["meter_timestamp"] = meterTs;
    }

    // Actief tarief (dal=1 / piek=2)
    if (d.electricity_tariff_present && d.electricity_tariff.length())
        r["tariff_indicator"] = d.electricity_tariff.toInt();

    // Stroomstoringen (cumulatieve tellers)
    if (d.electricity_failures_present)      r["power_failures"]      = d.electricity_failures;
    if (d.electricity_long_failures_present) r["long_power_failures"]  = d.electricity_long_failures;

    // Spanningskwaliteit per fase
    if (d.electricity_sags_l1_present)   r["voltage_sags_l1"]   = d.electricity_sags_l1;
    if (d.electricity_sags_l2_present)   r["voltage_sags_l2"]   = d.electricity_sags_l2;
    if (d.electricity_sags_l3_present)   r["voltage_sags_l3"]   = d.electricity_sags_l3;
    if (d.electricity_swells_l1_present) r["voltage_swells_l1"] = d.electricity_swells_l1;
    if (d.electricity_swells_l2_present) r["voltage_swells_l2"] = d.electricity_swells_l2;
    if (d.electricity_swells_l3_present) r["voltage_swells_l3"] = d.electricity_swells_l3;

    // Serienummers (idempotent op API — meter_serial/gas_meter_serial gaan op p1_meters)
    if (d.equipment_id_present     && d.equipment_id.length())     r["meter_serial"]     = d.equipment_id;
    if (d.gas_equipment_id_present && d.gas_equipment_id.length()) r["gas_meter_serial"] = d.gas_equipment_id;

    // Omgevingssensoren (HDC1080 temp + vocht) — nullable, alleen als geldig
    if (management.envValid()) {
        r["temp_c"] = round(management.envTempC() * 10.0f) / 10.0f;
        r["humid_pct"] = round(management.envHumidPct() * 10.0f) / 10.0f;
    }

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
// SPA gebruikt dit om de "Toon gegevens"-knop in Mijn Huis te vullen.
// Dubbele gate: skip alleen als IP onveranderd is t.o.v. NVS én laatste
// success <24h geleden was. Sinds backend-state-drift (DB-restore, row-loss)
// real is, garandeert de 24h-staleness zelf-recovery zonder reboot. Direct
// herstel kan via X-Mgmt-Refresh-signaal in de readings-respons (zie loop()).
// ============================================================================
void pushManagementInfo() {
    if (!networkReady() || apiKey.isEmpty() || baseUrl.isEmpty()) return;

    String currentIp = ethConnected ? ETH.localIP().toString() : WiFi.localIP().toString();
    String lastIp = prefs.getString("mgmt_ip", "");

    if (currentIp == lastIp) {
        time_t nowEpoch = time(nullptr);
        // Zonder NTP-sync kunnen we de leeftijd niet bepalen — vertrouw NVS
        // en skip, anders pushen we nodeloos op elke boot vóór NTP-sync.
        if (nowEpoch < 1700000000) return;
        uint32_t lastTs = prefs.getUInt("mgmt_ts", 0);
        if (lastTs != 0 && (uint32_t)nowEpoch - lastTs < MGMT_REFRESH_INTERVAL_S) {
            return;
        }
    }

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
                time_t nowEpoch = time(nullptr);
                if (nowEpoch >= 1700000000) {
                    prefs.putUInt("mgmt_ts", (uint32_t)nowEpoch);
                }
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

    // collectHeaders() bewaart alleen pointers — daarom static const.
    static const char* COLLECTED_HEADERS[] = {"X-Mgmt-Refresh"};

    for (;;) {
        esp_task_wdt_reset();
        PushJob* job = nullptr;
        if (xQueueReceive(pushQueue, &job, pdMS_TO_TICKS(5000)) != pdTRUE) {
            continue;  // periodieke wakeup voor WDT-reset
        }
        if (!job) continue;

        // Na WiFi/ETH reconnect is de TLS-socket stale — stop() hier zodat
        // http.begin() meteen een verse handshake doet (geen 5s timeout).
        if (forceSecureClientReset) {
            forceSecureClientReset = false;
            secureClient.stop();
        }

        HTTPClient http;
        http.setReuse(true);
        http.begin(secureClient, job->url);
        http.addHeader("Authorization", "Bearer " + apiKey);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("User-Agent", "slimhuys-p1/" FIRMWARE_VERSION);
        http.collectHeaders(COLLECTED_HEADERS, 1);

        int code = (job->method == "PUT")
                   ? http.PUT(job->payload)
                   : http.POST(job->payload);

        // Backend kan via X-Mgmt-Refresh: 1 op elke 2xx-respons signaleren
        // dat 'ie de mgmt-creds (host/admin_pass) kwijt is. Main-loop pakt
        // de flag op, clear't de NVS-gate en forceert pushManagementInfo().
        if (code >= 200 && code < 300) {
            String refresh = http.header("X-Mgmt-Refresh");
            if (refresh == "1") {
                mgmtRefreshRequested = true;
            }
        }

        http.end();

        // Bij upstream-uitval (modem/ISP weg terwijl ETH-link up blijft) merkt
        // secureClient niet dat de TLS-socket dood is — setReuse(true) probeert
        // 'm dan eindeloos opnieuw te gebruiken en alle volgende pushes falen.
        // Negatieve HTTPClient-codes = transport-fail → forceer verse handshake.
        if (code < 0) {
            secureClient.stop();
        }

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
#ifdef BOARD_V4
    // ESP32-C6 is single-core — xTaskCreatePinnedToCore bestaat niet.
    xTaskCreate(
        pushWorkerTask, "push-worker", 8192, nullptr,
        1 /* priority */, &pushTaskHandle);
#else
    xTaskCreatePinnedToCore(
        pushWorkerTask, "push-worker", 8192, nullptr,
        1 /* priority */, &pushTaskHandle, 0 /* core 0 */);
#endif
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
    r["flow_lpm"] = round(currentFlowLpm * 10.0f) / 10.0f;

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
    //
    // ALLEEN échte firmware-faults tellen als "crash". Een power-dip ("stroom
    // even weg") geeft een BROWNOUT-/EXT-/POWERON-reset — geen bug. Marginale
    // voeding kan tijdens boot een paar keer achter elkaar browne-outen (PHY +
    // ETH-init trekt stroompieken); die mochten ons voorheen ten onrechte in
    // safe-mode duwen, wat de P1-UART uitschakelt tot een handmatige herstart.
    bool faultReset = (bootResetReason == ESP_RST_PANIC ||
                       bootResetReason == ESP_RST_INT_WDT ||
                       bootResetReason == ESP_RST_TASK_WDT ||
                       bootResetReason == ESP_RST_WDT);

    Preferences bootPrefs;
    bootPrefs.begin("slimhuys", false);
    uint32_t crashCount = bootPrefs.getUInt("crash_count", 0);
    if (faultReset) {
        crashCount += 1;
        bootPrefs.putUInt("crash_count", crashCount);
    }
    // boot_count is monotonic — telt elke boot (i.t.t. crash_count die op
    // 60s stabiele uptime gereset wordt). Naar diagnostics-push.
    bootCount = bootPrefs.getUInt("boot_count", 0) + 1;
    bootPrefs.putUInt("boot_count", bootCount);
    bootPrefs.end();
    Serial.printf("Boot #%u (crashes-in-een-rij: %u, reset: %s%s)\n",
                  (unsigned)bootCount, (unsigned)crashCount,
                  resetReasonString(bootResetReason),
                  faultReset ? " [FAULT]" : "");
    if (crashCount >= BOOTLOOP_THRESHOLD) {
        safeMode = true;
        Serial.println("⚠️  SAFE-MODE — herhaalde crashes gedetecteerd");
        setLedMode(LedMode::ERROR);
    }

    // Persistente HTTPS-client zonder cert-pinning (slimhuys.nl is publieke
    // CA, een MITM op het lokale net is niet ons dreigingsmodel).
    secureClient.setInsecure();

    // WDT-timeout uitbreiden naar 30s vóórdat push-worker zich aanmeldt.
    // Arduino-ESP32 pre-initialiseert de TWDT met CONFIG_ESP_TASK_WDT_TIMEOUT_S
    // = 5s. De push-worker subscribeert via esp_task_wdt_add() en zijn
    // queue-wait is pdMS_TO_TICKS(5000) — exact gelijk aan die 5s. Race-to-crash
    // na precies 2 queue-cycli (~10s).
    // Hoofd-taak subscribeert pas ná setup (zie verderop) zodat captive-portal
    // onbeperkt kan lopen zonder WDT te triggeren.
#ifdef BOARD_V4
    // ESP-IDF 5.x: de TWDT is door de Arduino-core al geïnitialiseerd, dus
    // esp_task_wdt_init() geeft ESP_ERR_INVALID_STATE en wijzigt NIETS — de
    // timeout bleef daardoor op 5s staan en de push-worker tripte de WDT elke
    // cyclus. esp_task_wdt_reconfigure() updatet de bestaande TWDT (timeout +
    // panic-flag) in-place zonder subscriptions te wissen.
    {
        esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms = WDT_TIMEOUT_S * 1000,
            .idle_core_mask = 0,
            .trigger_panic = true,
        };
        if (esp_task_wdt_init(&wdt_cfg) == ESP_ERR_INVALID_STATE) {
            esp_task_wdt_reconfigure(&wdt_cfg);
        }
    }
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif

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

    // Ethernet EERST starten — WiFi.begin() vóór ETH.begin() kan op ESP32
    // de LAN8720 PHY-initialisatie breken (gedeelde RMII-registers).
    ethPhyInit();

    // STA-mode initialiseren + hostname zetten — vóór WiFi.begin() (saved
    // creds én later in het portal). Hostname blijft sticky over begin/disconnect.
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(deviceHostname.c_str());

    // Modem-sleep uit: dit is een wired-power device, dus de ~30mA
    // besparing weegt niet op tegen de WiFi-stabiliteit (voorkomt random
    // NOT_AUTHED-disconnects op DTIM-grenzen).
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    // Probeer opgeslagen WiFi-creds (parallel aan ethernet — wat 't eerst
    // up is, wordt gebruikt). Bij eerste boot zijn deze leeg → no-op.
    wifiSsid = prefs.getString("wifi_ssid", "");
    wifiPass = prefs.getString("wifi_pass", "");
    if (!wifiSsid.isEmpty()) {
        diagLog("WiFi begin: %s", wifiSsid.c_str());
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    } else {
        diagLog("WiFi: geen opgeslagen credentials");
    }

    // 45s wachten tot ETH óf WiFi up is. STP (Spanning Tree Protocol) houdt
    // nieuwe poorten 30-50s in blocking/learning-state; 45s dekt dat ruim af.
    unsigned long deadline = millis() + 45000;
    while (millis() < deadline && !networkReady()) {
        delay(100);
    }
    diagLog("Netwerk na wacht: ETH_link=%d ETH_ip=%s WiFi=%d",
            (int)ETH.linkUp(), ETH.localIP().toString().c_str(),
            (int)(WiFi.status() == WL_CONNECTED));

    // Safe-mode: skip captive-portal en remote-init. Management-UI komt
    // verderop wel up zodat de gebruiker factory-reset kan doen.
    //
    // Geprovisionede bridge (apiKey aanwezig) gaat NOOIT de portal in puur
    // door trage ETH — dat zet een rogue AP op en vereist on-site herstel.
    // Zonder apiKey is de portal noodzakelijk voor eerste pairing.
    if (!safeMode && apiKey.isEmpty()) {
        // Ongekoppeld apparaat in portal = geen crash. Anders bouwen herhaalde
        // portal-sessies (gebruiker komt later terug, vult code in, etc.) een
        // crash_count op die ons na 3× in safe-mode duwt — precies omgekeerd
        // van wat we willen tijdens onboarding.
        prefs.putUInt("crash_count", 0);
        Serial.println("Captive portal start (geen api-key — eerste pairing)");
        setLedMode(LedMode::PAIRING);  // blauw pulserend tot pairing klaar is
        CaptivePortal portal;
        String claimUrl = String(SLIMHUYS_BASE_URL) + "/v1/bridges/claim";
        // Ethernet-state doorgeven zodat het portaal alleen om de pairing-code
        // hoeft te vragen als de kabel al up is. timeoutMs=0 → portal blijft
        // draaien tot pairing succesvol — geen reboot-loop meer als de
        // gebruiker pas later z'n code intikt.
        portal.run("SlimHuys-Setup", claimUrl, ethConnected, 0);

        apiKey = portal.apiKey();
        baseUrl = portal.baseUrl();
        prefs.putString("api_key", apiKey);
        prefs.putString("base_url", baseUrl);
        // WiFi-creds alleen bewaren als de gebruiker ze heeft ingevuld —
        // anders zou de ethernet-flow de eventuele bestaande creds wissen.
        if (!portal.ssid().isEmpty()) {
            wifiSsid = portal.ssid();
            wifiPass = portal.password();
            prefs.putString("wifi_ssid", wifiSsid);
            prefs.putString("wifi_pass", wifiPass);
        }
        Serial.println("Credentials bewaard in NVS");

        // Portal draait in WIFI_AP_STA mode — terug naar STA en verbinding
        // maken met de opgeslagen creds zodat auto-reconnect actief wordt.
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        if (!wifiSsid.isEmpty()) {
            WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
        }
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
    // Idempotent — interne check skipt als IP onveranderd is t.o.v. NVS én
    // <24h geleden gepusht. Loop() doet de periodieke recheck.
    if (!safeMode) {
        pushManagementInfo();
        lastMgmtCheckAt = millis();
    }

    // P1-UART: 115200 8N1, RX-only (data komt naar ons toe).
    // Software-invert i.p.v. een NPN-inverter — moet ná begin() omdat
    // begin() de UART opnieuw configureert en de invert-flag overschrijft.
    // RX-buffer 2048B: DSMR 5.0-telegrammen kunnen ~900B groot zijn, default
    // 256B kan onder load overlopen en parses afbreken.
    if (!safeMode) {
        P1Serial.setRxBufferSize(2048);
        P1Serial.begin(115200, SERIAL_8N1, P1_RX_PIN, -1);
#ifdef BOARD_V4
        uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_RXD_INV);
#else
        uart_set_line_inverse(UART_NUM_2, UART_SIGNAL_RXD_INV);
#endif
        reader.enable(true);
    }

    // Hoofd-taak (loop) nu aanmelden bij WDT — ná de captive-portal zodat
    // een lange pairing-sessie de WDT niet triggert.
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

#ifndef BOARD_V4
    // V3 only: LAN8720 PHY power-cycle als driver draait maar na 15s nog geen link.
    // ETH.begin() NIET opnieuw (driver draait al, tweede aanroep geeft false).
    if (!ethPhyRetried && ethDriverStartedAt > 0
            && !ETH.linkUp() && millis() - ethDriverStartedAt >= 15000) {
        ethPhyRetried = true;
        diagLog("ETH: geen link na 15s — PHY power-cycle (driver blijft draaien)");
        pinMode(33, OUTPUT);
        digitalWrite(33, LOW);
        delay(500);
        digitalWrite(33, HIGH);
    }
#endif

    // ETH-DHCP-refresh: direct na ETH_CONNECTED (via vlag) en éénmalige
    // retry na 30s. Niet in de event-handler — interfereert met auto-DHCP.
    if (ethDhcpRefreshNeeded && ETH.linkUp() && !ethConnected) {
        ethDhcpRefreshNeeded = false;
        diagLog("ETH: DHCP-refresh (direct na link-up)");
        ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    if (!ethDhcpRetried && ETH.linkUp() && !ethConnected
            && ethLinkUpAt > 0 && millis() - ethLinkUpAt >= 30000) {
        ethDhcpRetried = true;
        diagLog("ETH: geen IP na 30s, DHCP retry (link=%d)", (int)ETH.linkUp());
        ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    // Eerste 60s: elke 5s ETH-linkstatus loggen voor diagnose.
    static unsigned long lastEthLog = 0;
    if (!ethConnected && millis() < 60000 && millis() - lastEthLog >= 5000) {
        lastEthLog = millis();
        diagLog("ETH poll: link=%d speed=%d", (int)ETH.linkUp(), (int)ETH.linkSpeed());
    }

    // WiFi-reconnect fallback: setAutoReconnect(true) handelt de meeste
    // gevallen af, maar na een mode-switch (portal → STA) of bij een
    // hardnekkige AP-uitval helpt een expliciete reconnect als belt-en-
    // bretels. Throttle op 60s zodat we de AP niet overspoelen.
    if (!wifiSsid.isEmpty() && WiFi.status() != WL_CONNECTED
            && !ethConnected && millis() - lastWifiReconnectAt >= 60000) {
        Serial.println("WiFi reconnect poging…");
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
        lastWifiReconnectAt = millis();
    }

    management.loop();
    if (safeMode) {
        // Last-resort self-recovery: na 4u stil in safe-mode 1× herstarten.
        // crash_count is door de 60s-grace al 0, dus dit boot normaal op.
        if (millis() >= SAFEMODE_AUTO_REBOOT_MS) {
            Serial.println("Safe-mode >4u — automatische herstart (self-recovery)");
            Serial.flush();
            ESP.restart();
        }
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

    // Management-info recheck. Twee triggers, gedeelde 60s-throttle:
    //  • Refresh-signaal van backend (X-Mgmt-Refresh: 1) — clear NVS-gate
    //    zodat pushManagementInfo() er gegarandeerd doorheen komt.
    //  • Periodiek 1×/u — pushManagementInfo() self-gate't intern op
    //    IP-equality + 24h-staleness, dus echt-pushen is zeldzaam.
    if (now - lastMgmtCheckAt >= MGMT_REFRESH_MIN_RETRY_MS) {
        if (mgmtRefreshRequested) {
            mgmtRefreshRequested = false;
            Serial.println("Backend vraagt mgmt-refresh — clear NVS-gate en re-push");
            prefs.remove("mgmt_ip");
            prefs.remove("mgmt_ts");
            pushManagementInfo();
            lastMgmtCheckAt = now;
        } else if (now - lastMgmtCheckAt >= MGMT_LOOP_CHECK_INTERVAL_MS) {
            pushManagementInfo();
            lastMgmtCheckAt = now;
        }
    }

    // LED-mode hertonen op basis van runtime-state. PAIRING wordt apart
    // gezet rond portal.run(); deze tak handelt LEAK/ERROR/OPERATIONAL.
    setLedMode(pickLedMode());

    // Cyan-flash op water-pulse — alleen in OPERATIONAL want andere modes
    // gebruiken al actief de blue/red kanalen.
    if (waterPulseFlashRequested) {
        waterPulseFlashRequested = false;
        if (currentLedMode == LedMode::OPERATIONAL) {
            ledcWrite(LED_B_ID, 80);    // moderate blue overlay → cyan met groen
            waterPulseFlashUntil = now + 50;
        }
    }
    if (waterPulseFlashUntil > 0 && now >= waterPulseFlashUntil) {
        waterPulseFlashUntil = 0;
        if (currentLedMode == LedMode::OPERATIONAL) {
            ledcWrite(LED_B_ID, 0);     // terug naar pure groen
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
