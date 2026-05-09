# slimhuys-p1-firmware

Open-source firmware voor de **SlimHuys P1+water-bridge**. Pusht DSMR-telegrammen,
water-pulses en lek-detectie naar [SlimHuys.nl](https://slimhuys.nl) en biedt
een lokale management-UI op `http://<bridge-ip>/`.

Eindklanten flashen via [slimhuys.nl/flash](https://slimhuys.nl/flash) — geen
hardware-werk, geen toolchain, alleen Chrome/Edge op desktop met USB-C-kabel.

## Hardware

Doelhardware: **[WaterP1MeterKit V3](https://waterp1meterkit.nl)** van smarthomeshop
(ESP32 + LAN8720 ethernet-PHY met PoE+, doorverkocht door SlimHuys met deze
firmware vooraf gefllasht). De kit bevat:

- DSMR P1-poort (RJ12 → slimme meter)
- Water-pulse-input (3-pin → reed-switch op de water-meter)
- Lek-sensor-input (active-low, voor moisture-pad)
- HDC1080 temp+vocht-sensor (I²C op de PCB)
- RGB-status-LED zichtbaar door de behuizing
- USB-C voeding + serial (CH343 → `/dev/cu.debug-console`)
- PoE+ alternatief voor stroom

### Pin-mapping

| GPIO | Functie |
|---|---|
| 16 | P1-data RX (software-inverted UART, 115200 8N1) |
| 12 | P1-request output (DTR) |
| 32 | Water-pulse input (reed-switch, FALLING-edge ISR) |
| 2  | Lek-sensor input (active-low, INPUT_PULLUP) |
| 15 / 4 | I²C SDA/SCL voor HDC1080 |
| 5 / 13 / 14 | RGB-LED rood/groen/blauw (LEDC PWM) |
| 23 / 18 / 17 / 33 | LAN8720 PHY: MDC / MDIO / CLK_OUT / PWR |

## Features

- **DSMR P1** — 1Hz push naar `/v1/me/readings` met voltage/current/power per fase, totalen, gas
- **Water-meter** — cumulatieve liter-stand + live flow (L/min via 10s sliding window) naar `/v1/me/water-readings`
- **Lek-detectie** — asymmetrische debounce (100ms aan / 2s uit), state-change push met exp.-backoff retry naar `/v1/me/bridges/leak`
- **HDC1080** — temp + luchtvochtigheid met self-heating-compensatie (-4.5°C / +12%)
- **Captive-portal pairing** — 6-cijferige code uit de SlimHuys-app; ethernet-aware (slaat WiFi-stap over als kabel up is)
- **OTA-updates** — periodiek check naar `/v1/firmware/manifest`, sha256-geverifieerd
- **Web-flasher** — [slimhuys.nl/flash](https://slimhuys.nl/flash) wijst naar `releases/latest/download/manifest.json` op deze repo (ESP Web Tools)
- **Management-UI** — `http://<bridge-ip>/` met live verbruik, push-status, factory-reset, handmatige OTA-upload
- **RGB-states** — groen=operationeel, blauw-pulse=pairing, rood=error, rood-blink=lekkage, cyan-flash=water-puls
- **Production-hardening** — 30s task-watchdog, bootloop-detectie (3× crash binnen 60s → safe-mode met alleen management-UI)
- **Diagnostics-push** — 1×/5min `{boot_count, uptime, heap, rssi, reset_reason, fw_version}` naar `/v1/me/bridges/diagnostics`
- **Dual-core** — alle HTTP-pushes op core 0 via FreeRTOS-queue zodat WebServer + sensors op core 1 nooit blokkeren op TLS-handshakes

## Eerste setup (eindklant)

1. **Pairing-code** aanmaken in de SlimHuys-app onder *Mijn Huis → P1-bridge koppelen* (10 min geldig).
2. **Bridge aansluiten**: PoE óf USB-C voor stroom, RJ12 in de P1-poort, optioneel water-pulse en/of lek-sensor.
3. **Flashen** via [slimhuys.nl/flash](https://slimhuys.nl/flash) — USB-C op laptop, klik *Install*.
4. **Captive-portal**: SSID `SlimHuys-Setup-XXXX`. Ethernet aangesloten → alleen pairing-code; anders WiFi-creds + code.
5. Klaar — status-LED wordt groen, data verschijnt direct in de SlimHuys-app.

Na een factory-reset is een nieuwe pairing-code nodig.

## Bouwen uit source

```bash
pip install platformio                                # eenmalig
pio run                                               # build
scripts/release.sh                                    # genereert release/<v>/
pio run -t upload --upload-port /dev/cu.debug-console # eerste flash
pio device monitor -b 115200                          # serial-log
```

Een tag `v*.*.*` op `main` triggert `.github/workflows/release.yml`, dat de
volgende artefacten als GitHub Release publiceert:

| Bestand | Doel |
|---|---|
| `firmware.bin` | OTA-app-image, geflasht naar offset 0x10000 |
| `firmware.bin.sha256` | sidecar voor integriteitscheck |
| `firmware-merged.bin` | full flash-image (offset 0x0) voor web-flasher |
| `manifest.json` | ESP Web Tools manifest met absolute GitHub-URL |

## Architectuur

```
┌─ CORE 1 (loopTask) ─────────────────┐     ┌─ CORE 0 (push-worker) ─┐
│  WebServer.handleClient()           │     │                        │
│  Sensors: P1 UART, leak, HDC, flow  │     │  xQueueReceive  ─────  │
│  reader.parse() → JSON build (5ms)  │ ──▶ │  HTTPClient.POST/PUT   │
│  enqueuePush() (microseconden)      │     │    └─ TLS-handshake    │
│  LED-state + cyan-flash             │     │  callback(http_code)   │
│  WDT reset                          │     │  WDT reset             │
└─────────────────────────────────────┘     └────────────────────────┘
       │                                              │
       │                                              ▼
       │                                       Persistente
       ▼                                       WiFiClientSecure
   Captive-portal,                             (TLS-session-cache)
   management-UI,
   sensor-fusion
```

Pushes worden in de main-loop *gebouwd* (~5ms JSON-serialize) en naar een
FreeRTOS-queue gestuurd. De worker-task op core 0 doet de HTTP-roundtrip
zonder ooit core 1 te blokkeren — WebServer-responses blijven vlot, sensor-
poll-cadens stabiel 1Hz.

## API-contracts

| Endpoint | Method | Wanneer |
|---|---|---|
| `/v1/me/readings` | POST | 1Hz P1-telegrammen |
| `/v1/me/water-readings` | POST | Bij verandering (max 1Hz), 60s heartbeat |
| `/v1/me/bridges/leak` | POST | Op state-change, retry-policy in callback |
| `/v1/me/bridges/management` | PUT | Bij pairing + IP-wijziging (idempotent) |
| `/v1/me/bridges/diagnostics` | POST | 1×/5min field-health |
| `/v1/firmware/manifest` | GET | Periodiek voor OTA-check |
| `/v1/bridges/claim` | POST | Eenmalig: code → api_key |

Alle pushes gebruiken Bearer-auth met de `api_key` uit NVS (geset bij eerste pairing).

## Repository-structuur

```
.
├── platformio.ini                    PIO-config (board=esp32dev, min_spiffs partities)
├── include/
│   ├── management.h                  WebServer-interface (UI + status-API)
│   ├── portal.h                      Captive-portal-class
│   └── updater.h                     OTA-updater
├── src/
│   ├── main.cpp                      Entry, loop, sensor-fusion, push-helpers
│   ├── management.cpp                /api/status JSON + embedded HTML/CSS/JS
│   ├── portal.cpp                    AP + DNS-hijack + setup-pagina
│   └── updater.cpp                   /v1/firmware/manifest poll + flash
├── scripts/
│   ├── merge_firmware.py             post-build: bouwt firmware-merged.bin
│   └── release.sh                    lokale release-bouw
└── .github/workflows/
    └── release.yml                   tag → GitHub Release met alle artefacten
```

## Licentie

MIT. Hardware komt van [smarthomeshop](https://smarthomeshop.io) (ook MIT-firmware,
maar die wordt overschreven door deze build).
