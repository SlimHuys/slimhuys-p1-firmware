#include "management.h"

#include <ArduinoJson.h>
#include <ETH.h>
#include <Update.h>
#include <WiFi.h>

// Diagnostiek-counter uit main.cpp — telt bytes op P1-UART
extern uint32_t p1BytesObserved;

extern const char MANAGEMENT_HTML[] PROGMEM;

void ManagementInterface::begin(Preferences* prefs, const String& version,
                                const String& adminPassword, OtaUpdater* updater) {
    _prefs = prefs;
    _version = version;
    _adminPass = adminPassword;
    _updater = updater;
    _setupRoutes();
    _server.begin();
}

void ManagementInterface::loop() {
    _server.handleClient();
}

void ManagementInterface::recordPush(int httpStatus) {
    _lastPushStatus = httpStatus;
    _lastPushAt = millis();
    if (httpStatus == 202 || httpStatus == 200) _pushOk++;
    else _pushFail++;
}

void ManagementInterface::recordWaterPush(int httpStatus) {
    _lastWaterPushStatus = httpStatus;
    _lastWaterPushAt = millis();
    if (httpStatus == 202 || httpStatus == 200) _waterPushOk++;
    else _waterPushFail++;
}

void ManagementInterface::recordParse(bool ok, const String& error) {
    _lastParseOk = ok;
    _lastParseAt = millis();
    if (ok) {
        _parseOk++;
        _lastParseError = "";
        _consecutiveParseFails = 0;
    } else {
        _parseFail++;
        _lastParseError = error;
        _consecutiveParseFails++;
    }
}

bool ManagementInterface::_requireAuth() {
    if (_adminPass.isEmpty()) return true;  // niet geconfigureerd → open (defensief)
    if (!_server.authenticate(_adminUser.c_str(), _adminPass.c_str())) {
        _server.requestAuthentication(BASIC_AUTH, "SlimHuys P1-bridge", "Authentication required");
        return false;
    }
    return true;
}

void ManagementInterface::_setupRoutes() {
    _server.on("/", HTTP_GET, [this]() { _handleRoot(); });
    _server.on("/api/status", HTTP_GET, [this]() { _handleStatus(); });
    _server.on("/api/reboot", HTTP_POST, [this]() { _handleReboot(); });
    _server.on("/api/repair", HTTP_POST, [this]() { _handleRepair(); });
    _server.on("/api/factory-reset", HTTP_POST, [this]() { _handleFactoryReset(); });
    _server.on("/api/ota", HTTP_POST,
               [this]() { _handleOtaPost(); },
               [this]() { _handleOtaUpload(); });
    _server.on("/api/check-update", HTTP_POST, [this]() { _handleCheckUpdate(); });
    _server.onNotFound([this]() { _server.send(404, "text/plain", "not found"); });
}

void ManagementInterface::_handleRoot() {
    _server.sendHeader("Cache-Control", "no-store");
    _server.send_P(200, "text/html; charset=utf-8", MANAGEMENT_HTML);
}

void ManagementInterface::_handleStatus() {
    JsonDocument doc;
    doc["version"] = _version;
    doc["uptime_s"] = millis() / 1000UL;
    doc["heap_free"] = ESP.getFreeHeap();

    JsonObject net = doc["network"].to<JsonObject>();
    if (ETH.linkUp()) {
        net["type"] = "ethernet";
        net["ip"] = ETH.localIP().toString();
        net["mac"] = ETH.macAddress();
    } else if (WiFi.status() == WL_CONNECTED) {
        net["type"] = "wifi";
        net["ssid"] = WiFi.SSID();
        net["rssi"] = WiFi.RSSI();
        net["ip"] = WiFi.localIP().toString();
        net["mac"] = WiFi.macAddress();
    } else {
        net["type"] = "none";
    }

    JsonObject p1 = doc["p1"].to<JsonObject>();
    p1["last_parse_ok"] = _lastParseOk;
    p1["last_parse_at_ms_ago"] = _lastParseAt > 0 ? (long)(millis() - _lastParseAt) : -1;
    p1["last_parse_error"] = _lastParseError;
    p1["parse_ok_count"] = _parseOk;
    p1["parse_fail_count"] = _parseFail;
    p1["consecutive_parse_fails"] = _consecutiveParseFails;
    p1["bytes_observed"] = p1BytesObserved;

    JsonObject api = doc["api"].to<JsonObject>();
    api["last_status"] = _lastPushStatus;
    api["last_at_ms_ago"] = _lastPushAt > 0 ? (long)(millis() - _lastPushAt) : -1;
    api["push_ok_count"] = _pushOk;
    api["push_fail_count"] = _pushFail;

    JsonObject water = doc["water"].to<JsonObject>();
    water["last_status"] = _lastWaterPushStatus;
    water["last_at_ms_ago"] = _lastWaterPushAt > 0 ? (long)(millis() - _lastWaterPushAt) : -1;
    water["push_ok_count"] = _waterPushOk;
    water["push_fail_count"] = _waterPushFail;

    if (_updater) {
        JsonObject ota = doc["ota"].to<JsonObject>();
        ota["channel"] = _updater->channel();
        ota["available_version"] = _updater->availableVersion();
        ota["available_notes"] = _updater->availableNotes();
        ota["last_check_ms_ago"] = _updater->lastCheckAtMsAgo();
    }

    JsonObject reading = doc["reading"].to<JsonObject>();
    reading["valid"] = _lastReading.valid;
    if (_lastReading.valid) {
        reading["at_ms_ago"] = (long)(millis() - _lastReading.at_ms);
        reading["active_power_w"] = _lastReading.active_power_w;
        reading["active_power_returned_w"] = _lastReading.active_power_returned_w;
        reading["voltage_l1"] = _lastReading.voltage_l1;
        reading["voltage_l2"] = _lastReading.voltage_l2;
        reading["voltage_l3"] = _lastReading.voltage_l3;
        reading["current_l1_a"] = _lastReading.current_l1_a;
        reading["current_l2_a"] = _lastReading.current_l2_a;
        reading["current_l3_a"] = _lastReading.current_l3_a;
        reading["active_power_l1_w"] = _lastReading.active_power_l1_w;
        reading["active_power_l2_w"] = _lastReading.active_power_l2_w;
        reading["active_power_l3_w"] = _lastReading.active_power_l3_w;
        reading["consumption_kwh"] = _lastReading.consumption_kwh;
        reading["delivered_kwh"] = _lastReading.delivered_kwh;
        reading["gas_m3"] = _lastReading.gas_m3;
        reading["water_l_total"] = _lastReading.water_l_total;
    }

    String body;
    serializeJson(doc, body);
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(200, "application/json", body);
}

void ManagementInterface::_handleReboot() {
    if (!_requireAuth()) return;
    _server.send(200, "application/json", "{\"status\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

void ManagementInterface::_handleRepair() {
    if (!_requireAuth()) return;
    if (_prefs) {
        _prefs->remove("api_key");
        _prefs->remove("base_url");
    }
    _server.send(200, "application/json", "{\"status\":\"repairing\"}");
    delay(500);
    ESP.restart();
}

void ManagementInterface::_handleFactoryReset() {
    if (!_requireAuth()) return;
    if (_prefs) {
        _prefs->clear();
    }
    _server.send(200, "application/json", "{\"status\":\"reset\"}");
    delay(500);
    ESP.restart();
}

void ManagementInterface::_handleOtaPost() {
    if (!_requireAuth()) return;
    if (Update.hasError()) {
        String err = "{\"error\":\"";
        err += Update.errorString();
        err += "\"}";
        _server.send(500, "application/json", err);
    } else {
        _server.send(200, "application/json", "{\"status\":\"updated\"}");
        delay(500);
        ESP.restart();
    }
}

void ManagementInterface::_handleCheckUpdate() {
    if (!_requireAuth()) return;
    if (!_updater) {
        _server.send(503, "application/json", "{\"error\":\"updater_not_initialized\"}");
        return;
    }

    OtaUpdater::Result r = _updater->checkNow();

    JsonDocument doc;
    switch (r) {
        case OtaUpdater::Result::UP_TO_DATE: doc["status"] = "up_to_date"; break;
        case OtaUpdater::Result::UPDATED_REBOOTING: doc["status"] = "rebooting"; break;
        case OtaUpdater::Result::ERROR_NETWORK: doc["status"] = "error"; doc["error"] = "network"; break;
        case OtaUpdater::Result::ERROR_MANIFEST: doc["status"] = "error"; doc["error"] = "manifest"; break;
        case OtaUpdater::Result::ERROR_DOWNLOAD: doc["status"] = "error"; doc["error"] = "download"; break;
        case OtaUpdater::Result::ERROR_HASH: doc["status"] = "error"; doc["error"] = "hash_mismatch"; break;
        case OtaUpdater::Result::ERROR_FLASH: doc["status"] = "error"; doc["error"] = "flash"; break;
    }
    if (!_updater->availableVersion().isEmpty()) {
        doc["available_version"] = _updater->availableVersion();
        doc["channel"] = _updater->channel();
        doc["notes"] = _updater->availableNotes();
    }

    String body;
    serializeJson(doc, body);
    _server.send(200, "application/json", body);
}

void ManagementInterface::_handleOtaUpload() {
    // Auth checken op start van upload — anders pakt de upload-handler chunks
    // op zonder dat de POST-handler überhaupt aangeroepen is.
    if (!_server.authenticate(_adminUser.c_str(), _adminPass.c_str())) return;

    HTTPUpload& upload = _server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("OTA klaar: %u bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println("OTA afgebroken");
    }
}

// =============================================================================
// Status- en beheerpagina
// =============================================================================
const char MANAGEMENT_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang="nl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#16a34a">
<title>SlimHuys P1-bridge</title>
<style>
  :root {
    --bg: #f8fafc;
    --surface: #ffffff;
    --border: #e2e8f0;
    --text: #0f172a;
    --muted: #64748b;
    --primary: #16a34a;
    --primary-hover: #15803d;
    --danger: #dc2626;
    --danger-hover: #b91c1c;
    --warning: #ea580c;
    --info: #2563eb;
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html, body { margin: 0; padding: 0; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif;
    background: var(--bg);
    color: var(--text);
    line-height: 1.5;
    min-height: 100vh;
    padding: max(env(safe-area-inset-top), 16px) 16px max(env(safe-area-inset-bottom), 16px);
  }
  .wrap { max-width: 460px; margin: 0 auto; }
  header { text-align: center; padding: 24px 0 16px; }
  .logo {
    height: 40px;
    width: auto;
    margin-bottom: 12px;
  }
  h1 { font-size: 22px; margin: 0 0 4px; letter-spacing: -0.02em; }
  .subtitle { color: var(--muted); font-size: 14px; margin: 0; }
  h2 {
    font-size: 13px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--muted);
    margin: 18px 4px 8px;
    font-weight: 600;
  }
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 6px 20px;
    margin-bottom: 12px;
  }
  .card.actions { padding: 20px; display: flex; flex-direction: column; gap: 10px; }
  .row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 14px 0;
    border-bottom: 1px solid var(--border);
    gap: 16px;
  }
  .row:last-child { border-bottom: none; }
  .row-label { color: var(--muted); font-size: 14px; flex: 0 0 auto; }
  .row-value {
    font-weight: 500; font-size: 14px;
    text-align: right;
    word-break: break-all;
    font-variant-numeric: tabular-nums;
  }
  .big-value {
    font-size: 28px;
    font-weight: 700;
    letter-spacing: -0.02em;
    text-align: center;
    padding: 10px 0 4px;
  }
  .big-value.consume { color: var(--danger); }
  .big-value.return { color: var(--primary); }
  .big-value.idle { color: var(--muted); }
  .big-label {
    text-align: center;
    font-size: 13px;
    color: var(--muted);
    padding-bottom: 12px;
  }
  .phase-grid {
    display: grid;
    grid-template-columns: 1fr 1fr 1fr;
    gap: 8px;
    padding: 12px 0;
  }
  .phase {
    text-align: center;
    background: var(--bg);
    border-radius: 10px;
    padding: 10px 6px;
  }
  .phase-name { font-size: 11px; color: var(--muted); font-weight: 600; }
  .phase-value { font-size: 16px; font-weight: 600; font-variant-numeric: tabular-nums; }
  .phase-unit { font-size: 11px; color: var(--muted); }
  .dot {
    display: inline-block;
    width: 8px; height: 8px;
    border-radius: 50%;
    margin-right: 6px;
    vertical-align: middle;
  }
  .dot.ok { background: var(--primary); }
  .dot.warn { background: var(--warning); }
  .dot.err { background: var(--danger); }
  .dot.idle { background: var(--muted); }
  button {
    width: 100%;
    padding: 12px;
    font-size: 15px; font-weight: 600;
    border: none;
    border-radius: 10px;
    cursor: pointer;
    font-family: inherit;
    transition: background 0.15s, transform 0.05s;
  }
  button:active { transform: scale(0.98); }
  .btn-secondary { background: var(--surface); color: var(--text); border: 1px solid var(--border); }
  .btn-secondary:hover { background: var(--bg); }
  .btn-info { background: var(--info); color: white; }
  .btn-info:hover { background: #1d4ed8; }
  .btn-warning { background: var(--warning); color: white; }
  .btn-warning:hover { background: #c2410c; }
  .btn-danger { background: var(--danger); color: white; }
  .btn-danger:hover { background: var(--danger-hover); }
  input[type="file"] {
    width: 100%;
    padding: 10px;
    font-size: 14px;
    background: var(--bg);
    border: 1px dashed var(--border);
    border-radius: 10px;
    margin-bottom: 10px;
  }
  .progress {
    width: 100%;
    height: 8px;
    background: var(--border);
    border-radius: 4px;
    overflow: hidden;
    margin-top: 10px;
    display: none;
  }
  .progress.active { display: block; }
  .progress-bar {
    height: 100%;
    background: var(--primary);
    transition: width 0.2s;
    width: 0;
  }
  .ota-status { font-size: 13px; color: var(--muted); margin-top: 8px; min-height: 18px; }
  footer {
    text-align: center;
    color: var(--muted);
    font-size: 12px;
    padding: 16px 0;
  }
  .hidden { display: none; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <svg class="logo" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 260 56" role="img" aria-label="SlimHuys.nl"><g transform="translate(4,4)"><rect x="8" y="30" width="6" height="12" rx="1.5" fill="#B8EBD2"/><rect x="16.67" y="26" width="6" height="16" rx="1.5" fill="#4FCB8E"/><rect x="25.33" y="22" width="6" height="20" rx="1.5" fill="#22B36B"/><rect x="34" y="20" width="6" height="22" rx="1.5" fill="#0E7A47"/><path d="M 4 20 L 24 6 L 44 20" fill="none" stroke="#042E1D" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/></g><text x="64" y="37" font-family="-apple-system,'Segoe UI',system-ui,sans-serif" font-size="26" font-weight="500" letter-spacing="-0.4"><tspan fill="#042E1D">Slim</tspan><tspan fill="#0E7A47">Huys</tspan><tspan fill="#4FCB8E" font-weight="400">.nl</tspan></text></svg>
    <h1>P1-bridge</h1>
    <p class="subtitle">Status &amp; beheer</p>
  </header>

  <h2>Live verbruik</h2>
  <div class="card" id="live-card">
    <div class="big-value idle" id="big-power">—</div>
    <div class="big-label" id="big-power-label">Wachten op telegram…</div>
    <div class="phase-grid" id="phase-grid">
      <div class="phase"><div class="phase-name">L1</div><div class="phase-value" id="vl1">—</div><div class="phase-unit">V</div></div>
      <div class="phase"><div class="phase-name">L2</div><div class="phase-value" id="vl2">—</div><div class="phase-unit">V</div></div>
      <div class="phase"><div class="phase-name">L3</div><div class="phase-value" id="vl3">—</div><div class="phase-unit">V</div></div>
    </div>
    <div class="row">
      <div class="row-label">Verbruikt totaal</div>
      <div class="row-value" id="kwh-cons">—</div>
    </div>
    <div class="row">
      <div class="row-label">Geleverd totaal</div>
      <div class="row-value" id="kwh-del">—</div>
    </div>
    <div class="row">
      <div class="row-label">Gas totaal</div>
      <div class="row-value" id="gas">—</div>
    </div>
    <div class="row">
      <div class="row-label">Water totaal</div>
      <div class="row-value" id="water">—</div>
    </div>
  </div>

  <h2>Status</h2>
  <div class="card">
    <div class="row">
      <div class="row-label">Netwerk</div>
      <div class="row-value" id="net">…</div>
    </div>
    <div class="row">
      <div class="row-label">IP-adres</div>
      <div class="row-value" id="ip">…</div>
    </div>
    <div class="row">
      <div class="row-label">Signaal</div>
      <div class="row-value" id="signal">…</div>
    </div>
    <div class="row">
      <div class="row-label">P1-data</div>
      <div class="row-value" id="p1">…</div>
    </div>
    <div class="row">
      <div class="row-label">P1 push</div>
      <div class="row-value" id="api">…</div>
    </div>
    <div class="row">
      <div class="row-label">Water push</div>
      <div class="row-value" id="water-api">…</div>
    </div>
    <div class="row">
      <div class="row-label">Uptime</div>
      <div class="row-value" id="uptime">…</div>
    </div>
  </div>

  <h2>Auto-update</h2>
  <div class="card">
    <div class="row">
      <div class="row-label">Channel</div>
      <div class="row-value" id="ota-channel">—</div>
    </div>
    <div class="row">
      <div class="row-label">Beschikbaar</div>
      <div class="row-value" id="ota-available">—</div>
    </div>
    <div class="row">
      <div class="row-label">Laatst gecheckt</div>
      <div class="row-value" id="ota-checked">—</div>
    </div>
  </div>
  <div class="card actions">
    <button class="btn-info" onclick="checkUpdate()" id="check-btn">Check nu op updates</button>
    <div class="ota-status" id="check-status"></div>
  </div>

  <h2>Handmatige firmware-upload</h2>
  <div class="card actions">
    <input type="file" id="ota-file" accept=".bin">
    <button class="btn-secondary" onclick="doOta()">Upload &amp; flash</button>
    <div class="progress" id="ota-progress"><div class="progress-bar" id="ota-bar"></div></div>
    <div class="ota-status" id="ota-status"></div>
  </div>

  <h2>Beheer</h2>
  <div class="card actions">
    <button class="btn-secondary" onclick="action('reboot')">Herstarten</button>
    <button class="btn-warning" onclick="action('repair')">Opnieuw koppelen</button>
    <button class="btn-danger" onclick="action('factory-reset')">Fabrieksinstellingen</button>
  </div>

  <footer>SlimHuys P1-bridge <span id="version">…</span></footer>
</div>

<script>
function fmtUptime(s) {
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s/60) + ' m';
  if (s < 86400) return Math.floor(s/3600) + ' u ' + Math.floor((s%3600)/60) + ' m';
  return Math.floor(s/86400) + ' d ' + Math.floor((s%86400)/3600) + ' u';
}
function fmtAgo(ms) {
  if (ms < 0) return 'nooit';
  if (ms < 1500) return 'zojuist';
  if (ms < 60000) return Math.floor(ms/1000) + ' s geleden';
  if (ms < 3600000) return Math.floor(ms/60000) + ' m geleden';
  return Math.floor(ms/3600000) + ' u geleden';
}
function dot(kind) { return '<span class="dot ' + kind + '"></span>'; }
function fmtNum(n, dec) {
  if (n === undefined || n === null) return '—';
  return Number(n).toLocaleString('nl-NL', {minimumFractionDigits: dec, maximumFractionDigits: dec});
}

function refresh() {
  fetch('/api/status').then(function(r){return r.json();}).then(function(d){
    var n = d.network;
    var netEl = document.getElementById('net');
    var sigEl = document.getElementById('signal');
    if (n.type === 'ethernet') {
      netEl.innerHTML = dot('ok') + 'Ethernet';
      sigEl.textContent = '–';
    } else if (n.type === 'wifi') {
      netEl.innerHTML = dot('ok') + 'WiFi · ' + n.ssid;
      sigEl.textContent = n.rssi + ' dBm';
    } else {
      netEl.innerHTML = dot('err') + 'Geen verbinding';
      sigEl.textContent = '–';
    }
    document.getElementById('ip').textContent = n.ip || '–';

    var p = d.p1;
    var p1El = document.getElementById('p1');
    if (p.last_parse_at_ms_ago < 0) {
      p1El.innerHTML = dot('idle') + 'Geen telegram ontvangen';
    } else if (p.last_parse_at_ms_ago > 5000) {
      p1El.innerHTML = dot('warn') + 'Stil sinds ' + fmtAgo(p.last_parse_at_ms_ago);
    } else if ((p.consecutive_parse_fails || 0) >= 3) {
      // Pas rood na 3 fails op rij — voorkomt flikker bij occasionele DSMR-glitches
      p1El.innerHTML = dot('err') + 'Parse-fout: ' + (p.last_parse_error || '?');
    } else {
      p1El.innerHTML = dot('ok') + p.parse_ok_count + ' OK · ' + p.parse_fail_count + ' fout';
    }

    function renderPush(el, x, idleLabel) {
      if (x.last_at_ms_ago < 0) {
        el.innerHTML = dot('idle') + idleLabel;
        return;
      }
      var ok = (x.last_status === 200 || x.last_status === 202);
      el.innerHTML = dot(ok ? 'ok' : 'err')
        + 'HTTP ' + x.last_status + ' · ' + fmtAgo(x.last_at_ms_ago)
        + ' (' + x.push_ok_count + '/' + (x.push_ok_count + x.push_fail_count) + ')';
    }
    renderPush(document.getElementById('api'),       d.api,   'Nog niet gepusht');
    renderPush(document.getElementById('water-api'), d.water, 'Nog niet gepusht');

    document.getElementById('uptime').textContent = fmtUptime(d.uptime_s);
    document.getElementById('version').textContent = 'v' + d.version;

    if (d.ota) {
      document.getElementById('ota-channel').textContent = d.ota.channel || 'stable';
      var avail = d.ota.available_version;
      if (avail && avail !== d.version) {
        document.getElementById('ota-available').innerHTML = dot('warn') + 'v' + avail + (d.ota.available_notes ? ' · ' + d.ota.available_notes : '');
      } else {
        document.getElementById('ota-available').innerHTML = dot('ok') + 'Up-to-date';
      }
      document.getElementById('ota-checked').textContent = d.ota.last_check_ms_ago < 0 ? 'nooit' : fmtAgo(d.ota.last_check_ms_ago);
    }

    // Live reading
    var r = d.reading;
    var bigEl = document.getElementById('big-power');
    var labelEl = document.getElementById('big-power-label');
    if (r && r.valid && r.at_ms_ago < 5000) {
      var w = r.active_power_w;
      bigEl.textContent = fmtNum(Math.abs(w), 0) + ' W';
      if (w >= 0) {
        bigEl.className = 'big-value consume';
        labelEl.textContent = 'wordt verbruikt';
      } else {
        bigEl.className = 'big-value return';
        labelEl.textContent = 'wordt teruggeleverd';
      }
      document.getElementById('vl1').textContent = fmtNum(r.voltage_l1, 1);
      document.getElementById('vl2').textContent = fmtNum(r.voltage_l2, 1);
      document.getElementById('vl3').textContent = fmtNum(r.voltage_l3, 1);
      document.getElementById('kwh-cons').textContent = fmtNum(r.consumption_kwh, 3) + ' kWh';
      document.getElementById('kwh-del').textContent = fmtNum(r.delivered_kwh, 3) + ' kWh';
      document.getElementById('gas').textContent = r.gas_m3 ? (fmtNum(r.gas_m3, 3) + ' m³') : '—';
      // Water: ESP32 telt liters; toon als L < 1000, anders m³
      var waterL = r.water_l_total || 0;
      document.getElementById('water').textContent = waterL < 1000
        ? (fmtNum(waterL, 0) + ' L')
        : (fmtNum(waterL / 1000, 3) + ' m³');
    } else {
      bigEl.textContent = '—';
      bigEl.className = 'big-value idle';
      labelEl.textContent = r && r.valid ? 'Stil sinds ' + fmtAgo(r.at_ms_ago) : 'Wachten op telegram…';
    }
  }).catch(function(){});
}

function action(kind) {
  var prompts = {
    'reboot': ['Bridge herstarten?', null, 'Herstart bezig — pagina laadt over ~10 seconden vanzelf.'],
    'repair': ['De bridge ontkoppelen en opnieuw koppelen?', 'Je hebt straks een nieuwe pairing-code nodig uit de SlimHuys-app.', 'Bridge gaat in setup-mode. Verbind met de SlimHuys-Setup-WiFi om opnieuw in te stellen.'],
    'factory-reset': ['Alles wissen?', 'Dit verwijdert WiFi-creds én api-key. De bridge gaat helemaal opnieuw setup doen.', 'Bridge gewist. Verbind met de SlimHuys-Setup-WiFi om opnieuw in te stellen.'],
  };
  var p = prompts[kind];
  if (!confirm(p[0])) return;
  if (p[1] && !confirm(p[1])) return;
  fetch('/api/' + kind, {method: 'POST'}).then(function(res){
    if (res.status === 401) { alert('Fout login. Refresh de pagina en probeer opnieuw.'); return; }
    alert(p[2]);
    if (kind === 'reboot') setTimeout(function(){ location.reload(); }, 10000);
  });
}

function checkUpdate() {
  var btn = document.getElementById('check-btn');
  var statusEl = document.getElementById('check-status');
  btn.disabled = true;
  statusEl.textContent = 'Bezig met checken…';
  fetch('/api/check-update', {method: 'POST'}).then(function(r){
    if (r.status === 401) { statusEl.textContent = 'Login vereist.'; btn.disabled = false; return; }
    return r.json();
  }).then(function(d){
    if (!d) return;
    if (d.status === 'rebooting') {
      statusEl.textContent = 'Update gevonden — flashen + herstarten…';
      setTimeout(function(){ location.reload(); }, 30000);
    } else if (d.status === 'up_to_date') {
      statusEl.textContent = 'Geen update beschikbaar.';
      btn.disabled = false;
    } else {
      statusEl.textContent = 'Fout: ' + (d.error || 'onbekend');
      btn.disabled = false;
    }
    refresh();
  }).catch(function(){
    statusEl.textContent = 'Verbinding verbroken.';
    btn.disabled = false;
  });
}

function doOta() {
  var fileInput = document.getElementById('ota-file');
  var statusEl = document.getElementById('ota-status');
  var progEl = document.getElementById('ota-progress');
  var barEl = document.getElementById('ota-bar');
  if (!fileInput.files.length) { statusEl.textContent = 'Kies eerst een firmware.bin-bestand.'; return; }
  if (!confirm('Firmware uploaden en flashen? Bridge herstart na succes.')) return;

  var fd = new FormData();
  fd.append('firmware', fileInput.files[0]);
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.upload.addEventListener('progress', function(e){
    if (e.lengthComputable) {
      var pct = (e.loaded / e.total * 100).toFixed(0);
      barEl.style.width = pct + '%';
      statusEl.textContent = 'Uploaden… ' + pct + '%';
    }
  });
  xhr.onloadstart = function(){ progEl.classList.add('active'); barEl.style.width = '0'; };
  xhr.onload = function(){
    if (xhr.status === 401) {
      statusEl.textContent = 'Fout login.';
    } else if (xhr.status === 200) {
      barEl.style.width = '100%';
      statusEl.textContent = 'Geflasht — bridge herstart. Pagina laadt over 15s.';
      setTimeout(function(){ location.reload(); }, 15000);
    } else {
      statusEl.textContent = 'Fout (HTTP ' + xhr.status + '): ' + xhr.responseText;
    }
  };
  xhr.onerror = function(){ statusEl.textContent = 'Verbinding verbroken.'; };
  xhr.send(fd);
}

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)=====";
