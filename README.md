# slimhuys-p1-firmware

Open firmware voor een P1-bridge die DSMR-telegrammen van een Nederlandse
slimme meter doorzet naar [SlimHuys.nl](https://slimhuys.nl). Doelhardware:
**WT32-ETH01** (ESP32 + ethernet) of varianten op ESP32-C3/S3 met WiFi.

> **Status**: pre-release. Werkt op breadboard met testen pending eerste
> echte hardware-fit.

## Bouwen — onderdelen

Voor één prototype:

| Onderdeel | TinyTronics SKU | Prijs |
|---|---|---|
| WT32-ETH01 ESP32 ethernet board | 006083 | €13,50 |
| RJ12 6P6C P1-poort kabel 50cm | 007296 | €3,25 |
| NPN-transistor 2N3904 (10×) | 000116 | €1,50 |
| 10kΩ weerstand (10×) | 000138 | €0,50 |
| Breadboard 830 punten | 006327 | €5,00 |
| DuPont jumpers Male-Male 100× | 002977 | €4,50 |
| CH340E TTL USB-serieel | 004493 | €2,50 |

**Totaal: ~€31** + verzending.

## Schematic — level-converter + P1-input

DSMR P1-poort levert een **5V geïnverteerd** UART-signaal. We converteren
dat naar non-inverted 3.3V met een NPN-transistor + 2 weerstanden:

```
                                                            +3.3V
                                                              │
                                                              R2 (10kΩ)
                                                              │
   P1-RJ12 (slimme meter)                       ┌─── GPIO5 ──┴─── ESP32 RX2
   ┌─────────────────┐                          │
   │ pin 1: +5V ─────┼──┐                       │
   │ pin 2: DTR  ────┼──┼─── GPIO33 (request)   │
   │ pin 3: GND ─────┼──┼──────────┬────────────┤
   │ pin 4: N/C      │  │          │            │
   │ pin 5: RX-DATA ─┼──┼──┐       │   collector│
   │ pin 6: GND      │  │  │       │           ╱│
   └─────────────────┘  │  │       │      base╱ │  2N3904
                        │  │       │      ───┤  │
                        │  │       │          ╲ │
                        │  │       │  emitter  ╲│
                        │  │       │            └────────── GND
                        │  │       │
                        │  │     R1 (10kΩ)
                        │  │       │
                        │  │       └─── DATA-IN (base via R1)
                        │  │
                        ▼  ▼
                   USB-C 5V naar ESP32 VIN (NIET via P1-pin1; te weinig
                   stroom voor WT32-ETH01 onder ethernet-load)
```

**Pin-mapping**:

| WT32-ETH01 pin | Functie |
|---|---|
| GPIO 5 | P1-RX (uit transistor-collector) |
| GPIO 33 | P1-DTR (request-pin, naar slimme-meter pin 2) |
| GND | gemeenschappelijke ground |
| 5V (VIN) | externe USB-C-voeding |

## Firmware bouwen + flashen

```bash
# Eenmalig: PlatformIO installeren
pip install platformio
# of via VSCode-extension "PlatformIO IDE"

# Bouwen
pio run

# Flashen (eerste keer via USB-TTL adapter)
pio run -t upload --upload-port /dev/cu.usbserial-XXXX

# Serial-log lezen
pio device monitor -b 115200
```

**Eerste-keer flashen via CH340 TTL-adapter** (de WT32-ETH01 heeft geen
USB onboard):

| CH340 pin | WT32-ETH01 pin |
|---|---|
| TX | RX0 |
| RX | TX0 |
| GND | GND |
| 3V3 | 3V3 |
| (handmatig) | EN naar GND voor reset |
| (handmatig) | IO0 naar GND tijdens reset = flash-mode |

Volgende updates kunnen via OTA over het netwerk — zie `platformio.ini`.

## Eerste setup

1. **Open SlimHuys** in je browser → Mijn Huis → "**+ Pairing-code aanmaken**".
   Je krijgt een 6-cijferige code (10 min geldig).
2. **Flash firmware** op je WT32-ETH01.
3. **Steek USB-C voeding erin** (niet uit P1-poort — te weinig stroom voor ethernet).
4. **Optioneel: ethernet-kabel** aansluiten. Werkt ook puur op WiFi.
5. **Verbind met `SlimHuys-Setup`-WiFi** vanaf je telefoon (ESP32 hosting AP):
   - Kies je eigen WiFi-netwerk + wachtwoord (zelfs als je ethernet hebt — dat is voor permanent).
   - Vul de **6-cijferige pairing-code** in.
   - Submit.
6. Device wisselt code in voor api-key, bewaart in NVS, en de SlimHuys-app
   springt automatisch naar "**Bridge gekoppeld ✓**".
7. **Plug RJ12-kabel in je P1-poort**. Status-LED blijft aan = data flow OK.

Vanaf nu boot device zelfstandig met opgeslagen credentials. Een nieuwe
pairing-code is alleen nodig na een factory-reset.

## Architectuur

```
┌──────────────┐     ┌─────────────────┐    ┌──────────────────┐
│ slimme meter │ P1  │ WT32-ETH01      │    │ slimhuys.nl      │
│              ├────▶│  + DSMR-parser  ├───▶│ /v1/me/readings  │
│  (DSMR v5)   │ 1Hz │  + HTTPS POST   │ 1Hz│  + Reverb-WS     │
└──────────────┘     └─────────────────┘    └──────────────────┘
                            ▲                          │
                            │ /v1/bridges/claim        ▼
                            │ (eerste setup)   ┌──────────────────┐
                            │                  │ SlimHuys-dashboard│
                            │                  │  realtime ~1Hz   │
                       6-cijferige             └──────────────────┘
                       pairing-code
                       uit SlimHuys-app
```

Velden die we doorzetten matchen 1-op-1 met de
[HACS-integratie](https://github.com/SlimHuys/slimhuys-homeassistant) v0.4.0:
voltage L1/L2/L3, current L1/L2/L3, active_power_l1/l2/l3 (consumed),
active_power_returned_l1/l2/l3 (returned), gas_total_m3, plus de basics
(consumption_kwh_total, delivered_kwh_total, active_power_w).

## Licentie

MIT. Hardware-design (PCB later) onder CERN-OHL-S.
