#include "portal.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

extern const char PORTAL_HTML[] PROGMEM;

bool CaptivePortal::run(const char* apSsid, const String& claimUrl, bool ethReady,
                        unsigned long timeoutMs) {
    _claimUrl = claimUrl;
    _ethReady = ethReady;
    _state = State::IDLE;
    _stateChangedAt = millis();

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSsid);
    delay(100);

    IPAddress ip = WiFi.softAPIP();
    Serial.printf("Portal AP '%s' op %s\n", apSsid, ip.toString().c_str());

    _dns.start(53, "*", ip);
    _setupRoutes();
    _server.begin();

    // Eerste scan triggeren — duurt ~3s, daarna asynchroon refreshen.
    // Ethernet-flow heeft geen WiFi-keuzemenu, dus skip de scans.
    if (!_ethReady) {
        WiFi.scanNetworks(true);
        _lastScanAt = millis();
    }

    // timeoutMs=0 → portal blijft draaien tot pairing succesvol is. Eerste-
    // setup heeft geen tijdsdruk; een timeout-reboot zou alleen de gebruiker
    // wegjagen die net z'n code aan 't intikken is.
    bool noTimeout = (timeoutMs == 0);
    unsigned long deadline = millis() + timeoutMs;
    while (noTimeout || millis() < deadline) {
        _dns.processNextRequest();
        _server.handleClient();
        _processBackground();

        // Periodiek opnieuw scannen zolang user nog op portal-pagina is
        if (!_ethReady && _state == State::IDLE && millis() - _lastScanAt > 15000) {
            int n = WiFi.scanComplete();
            if (n != WIFI_SCAN_RUNNING) {
                WiFi.scanDelete();
                WiFi.scanNetworks(true);
                _lastScanAt = millis();
            }
        }

        if (_state == State::SUCCESS) {
            // Even tijd geven voor browser om success-bericht te ontvangen
            unsigned long graceUntil = millis() + 2000;
            while (millis() < graceUntil) {
                _dns.processNextRequest();
                _server.handleClient();
                delay(1);
            }
            _server.stop();
            _dns.stop();
            return true;
        }

        delay(1);
    }

    _server.stop();
    _dns.stop();
    return false;
}

void CaptivePortal::_setupRoutes() {
    _server.on("/", HTTP_GET, [this]() { _handleRoot(); });
    _server.on("/state", HTTP_GET, [this]() {
        // Vroeg in pageload: portal-page checkt of WiFi-velden nodig zijn.
        String body = String("{\"eth_ready\":") + (_ethReady ? "true" : "false") + "}";
        _server.sendHeader("Cache-Control", "no-store");
        _server.send(200, "application/json", body);
    });
    _server.on("/scan", HTTP_GET, [this]() { _handleScan(); });
    _server.on("/save", HTTP_POST, [this]() { _handleSave(); });
    _server.on("/status", HTTP_GET, [this]() { _handleStatus(); });

    // Captive-portal-detectie van iOS / Android / Windows
    _server.on("/hotspot-detect.html", HTTP_GET, [this]() { _handleProbe(); });
    _server.on("/library/test/success.html", HTTP_GET, [this]() { _handleProbe(); });
    _server.on("/generate_204", HTTP_GET, [this]() { _handleProbe(); });
    _server.on("/gen_204", HTTP_GET, [this]() { _handleProbe(); });
    _server.on("/connecttest.txt", HTTP_GET, [this]() { _handleProbe(); });
    _server.on("/ncsi.txt", HTTP_GET, [this]() { _handleProbe(); });
    _server.on("/redirect", HTTP_GET, [this]() { _handleProbe(); });

    _server.onNotFound([this]() { _handleProbe(); });
}

void CaptivePortal::_handleRoot() {
    _server.sendHeader("Cache-Control", "no-store");
    _server.send_P(200, "text/html; charset=utf-8", PORTAL_HTML);
}

void CaptivePortal::_handleScan() {
    int n = WiFi.scanComplete();

    JsonDocument doc;
    if (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED) {
        doc["scanning"] = true;
        doc["networks"].to<JsonArray>();
    } else {
        doc["scanning"] = false;
        JsonArray nets = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.isEmpty()) continue;
            JsonObject net = nets.add<JsonObject>();
            net["ssid"] = ssid;
            net["rssi"] = WiFi.RSSI(i);
            net["secured"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
    }
    String body;
    serializeJson(doc, body);
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(200, "application/json", body);
}

void CaptivePortal::_handleSave() {
    if (!_server.hasArg("code")) {
        _server.send(400, "application/json", "{\"error\":\"missing_code\"}");
        return;
    }
    _code = _server.arg("code");
    if (_code.length() != 6) {
        _server.send(400, "application/json", "{\"error\":\"invalid_code\"}");
        return;
    }

    _ssid = _server.hasArg("ssid") ? _server.arg("ssid") : "";
    _password = _server.hasArg("password") ? _server.arg("password") : "";

    // SSID alleen verplicht als er geen ethernet is — anders kan claim direct
    // over de kabel.
    if (!_ethReady && _ssid.isEmpty()) {
        _server.send(400, "application/json", "{\"error\":\"missing_ssid\"}");
        return;
    }

    // Direct ack; werk gebeurt async in _processBackground().
    _stateChangedAt = millis();
    if (_ssid.isEmpty()) {
        // Ethernet-only: skip WiFi-connect, ga direct claimen
        _server.send(200, "application/json", "{\"status\":\"pairing\"}");
        _state = State::PAIRING;
    } else {
        _server.send(200, "application/json", "{\"status\":\"connecting_wifi\"}");
        _state = State::CONNECTING_WIFI;
        WiFi.disconnect();
        delay(100);
        WiFi.begin(_ssid.c_str(), _password.c_str());
    }
}

void CaptivePortal::_handleStatus() {
    JsonDocument doc;
    switch (_state) {
        case State::IDLE: doc["status"] = "idle"; break;
        case State::CONNECTING_WIFI: doc["status"] = "connecting_wifi"; break;
        case State::PAIRING: doc["status"] = "pairing"; break;
        case State::SUCCESS: doc["status"] = "success"; break;
        case State::ERROR_WIFI:
            doc["status"] = "error";
            doc["error"] = "wifi";
            doc["message"] = _errorMsg;
            break;
        case State::ERROR_PAIRING:
            doc["status"] = "error";
            doc["error"] = "pairing";
            doc["message"] = _errorMsg;
            break;
    }
    String body;
    serializeJson(doc, body);
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(200, "application/json", body);
}

void CaptivePortal::_handleProbe() {
    // Captive-portal probes en losse 404's redirecten naar de portal-root
    String url = "http://" + WiFi.softAPIP().toString() + "/";
    _server.sendHeader("Location", url, true);
    _server.send(302, "text/plain", "");
}

void CaptivePortal::_processBackground() {
    if (_state == State::CONNECTING_WIFI) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("WiFi: %s @ %s\n", _ssid.c_str(), WiFi.localIP().toString().c_str());
            _state = State::PAIRING;
            _stateChangedAt = millis();
            if (_claim()) {
                _state = State::SUCCESS;
            } else {
                _state = State::ERROR_PAIRING;
            }
        } else if (millis() - _stateChangedAt > 30000) {
            _state = State::ERROR_WIFI;
            _errorMsg = "Verbinden duurde te lang. Klopt het wachtwoord?";
            WiFi.disconnect();
        }
    } else if (_state == State::PAIRING) {
        // Ethernet-only flow: claim direct (de WiFi-tak transitioneert PAIRING
        // intern binnen één iteratie, dus deze branch raakt alleen ethernet).
        if (_claim()) {
            _state = State::SUCCESS;
        } else {
            _state = State::ERROR_PAIRING;
        }
    }
}

bool CaptivePortal::_claim() {
    HTTPClient http;
    http.begin(_claimUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "slimhuys-p1/" FIRMWARE_VERSION);

    JsonDocument req;
    req["code"] = _code;
    String reqBody;
    serializeJson(req, reqBody);

    int status = http.POST(reqBody);
    String respBody = http.getString();
    http.end();

    if (status != 200) {
        if (status == 422 || status == 404) {
            _errorMsg = "Code is ongeldig of verlopen. Vraag een nieuwe aan in de SlimHuys-app.";
        } else if (status < 0) {
            _errorMsg = "Geen verbinding met SlimHuys. Probeer 't zo nogmaals.";
        } else {
            _errorMsg = "Onverwachte fout (HTTP " + String(status) + ").";
        }
        Serial.printf("Claim faalde: HTTP %d — %s\n", status, respBody.c_str());
        return false;
    }

    JsonDocument resp;
    if (deserializeJson(resp, respBody)) {
        _errorMsg = "Ongeldig antwoord van server.";
        return false;
    }

    _apiKey = resp["api_key"].as<String>();
    _baseUrl = resp["base_url"].as<String>();
    if (_apiKey.isEmpty() || _baseUrl.isEmpty()) {
        _errorMsg = "Server-response mist api_key of base_url.";
        return false;
    }

    Serial.println("Pairing succesvol — credentials ontvangen");
    return true;
}

// =============================================================================
// Embedded portal-pagina — single-page-app, vanilla JS, geen externe deps
// =============================================================================
const char PORTAL_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang="nl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#16a34a">
<title>SlimHuys – P1-bridge instellen</title>
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
    --danger-bg: #fef2f2;
    --success: #16a34a;
    --success-bg: #f0fdf4;
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
  header { text-align: center; padding: 24px 0 20px; }
  .logo {
    height: 40px;
    width: auto;
    margin-bottom: 12px;
  }
  h1 { font-size: 22px; margin: 0 0 4px; letter-spacing: -0.02em; }
  .subtitle { color: var(--muted); font-size: 14px; margin: 0; }
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 20px;
    margin-bottom: 12px;
  }
  .field { margin-bottom: 18px; }
  .field:last-child { margin-bottom: 0; }
  label {
    display: block;
    font-size: 14px; font-weight: 500;
    margin-bottom: 6px;
  }
  .hint {
    font-size: 13px; color: var(--muted);
    margin: -2px 0 8px;
  }
  select, input[type="password"], input[type="text"] {
    width: 100%;
    padding: 12px 14px;
    font-size: 16px;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: var(--surface);
    color: var(--text);
    font-family: inherit;
    -webkit-appearance: none;
    appearance: none;
  }
  select { cursor: pointer; }
  select:focus, input:focus {
    outline: none;
    border-color: var(--primary);
    box-shadow: 0 0 0 3px rgba(22,163,74,0.16);
  }
  .code {
    letter-spacing: 0.5em;
    text-align: center;
    font-size: 22px;
    font-variant-numeric: tabular-nums;
    font-weight: 600;
    padding-left: 0.5em;
  }
  button {
    width: 100%;
    padding: 14px;
    font-size: 16px; font-weight: 600;
    background: var(--primary);
    color: white;
    border: none;
    border-radius: 10px;
    cursor: pointer;
    font-family: inherit;
    transition: background 0.15s, transform 0.05s;
  }
  button:hover:not(:disabled) { background: var(--primary-hover); }
  button:active:not(:disabled) { transform: scale(0.98); }
  button:disabled {
    background: var(--border);
    color: var(--muted);
    cursor: not-allowed;
  }
  .status .icon {
    width: 56px; height: 56px;
    border-radius: 50%;
    margin: 0 auto 14px;
    display: flex; align-items: center; justify-content: center;
  }
  .status.success .icon { background: var(--success-bg); color: var(--success); }
  .status.error .icon { background: var(--danger-bg); color: var(--danger); }
  .status .icon.spinner-wrap { background: #f1f5f9; }
  .status .icon svg { width: 28px; height: 28px; }
  .spinner {
    width: 28px; height: 28px;
    border: 3px solid #cbd5e1;
    border-top-color: var(--primary);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
  .status { text-align: center; }
  .status h2 { margin: 0 0 4px; font-size: 18px; letter-spacing: -0.01em; }
  .status p { margin: 0; color: var(--muted); font-size: 14px; }
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
    <h1>P1-bridge instellen</h1>
    <p class="subtitle" id="subtitle">Verbind je SlimHuys-bridge met je netwerk</p>
  </header>

  <div id="form-card" class="card">
    <div class="field" id="wifi-fields">
      <label for="ssid">WiFi-netwerk</label>
      <select id="ssid"><option value="" disabled selected>Netwerken zoeken…</option></select>
    </div>

    <div class="field" id="pass-field">
      <label for="password">WiFi-wachtwoord</label>
      <input type="password" id="password" autocomplete="off">
    </div>

    <div class="field">
      <label for="code">Pairing-code</label>
      <p class="hint">6 cijfers, te vinden in de SlimHuys-app onder<br>"Mijn Huis → P1-bridge koppelen"</p>
      <input type="text" id="code" class="code" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" autocomplete="off" placeholder="000000">
    </div>

    <button id="submit" disabled>Verbinden</button>
  </div>

  <div id="status-card" class="card status hidden">
    <div class="icon spinner-wrap"><div class="spinner"></div></div>
    <h2 id="status-title">Bezig…</h2>
    <p id="status-message"></p>
  </div>

  <footer>SlimHuys P1-bridge</footer>
</div>

<script>
(function() {
  var ssidEl = document.getElementById('ssid');
  var passEl = document.getElementById('password');
  var codeEl = document.getElementById('code');
  var btnEl = document.getElementById('submit');
  var formEl = document.getElementById('form-card');
  var statusEl = document.getElementById('status-card');
  var titleEl = document.getElementById('status-title');
  var msgEl = document.getElementById('status-message');
  var wifiFieldsEl = document.getElementById('wifi-fields');
  var passFieldEl = document.getElementById('pass-field');
  var subtitleEl = document.getElementById('subtitle');

  // ethReady wordt door /state gezet vóór updateBtn() betekenis krijgt.
  // Default false (= WiFi nodig) zodat het oude gedrag blijft als /state faalt.
  var ethReady = false;

  function loadState() {
    fetch('/state').then(function(r){return r.json();}).then(function(d){
      ethReady = !!d.eth_ready;
      if (ethReady) {
        wifiFieldsEl.classList.add('hidden');
        passFieldEl.classList.add('hidden');
        subtitleEl.textContent = 'Vul je pairing-code in om je bridge te koppelen';
      } else {
        loadNetworks();
      }
      updateBtn();
    }).catch(function(){
      // Fallback: ga uit van WiFi-flow
      loadNetworks();
    });
  }

  function loadNetworks() {
    fetch('/scan').then(function(r){return r.json();}).then(function(d){
      if (d.scanning) { setTimeout(loadNetworks, 1500); return; }
      if (!d.networks || !d.networks.length) {
        ssidEl.innerHTML = '<option value="" disabled selected>Geen netwerken gevonden</option>';
        return;
      }
      d.networks.sort(function(a,b){return b.rssi - a.rssi;});
      var seen = {};
      var prev = ssidEl.value;
      ssidEl.innerHTML = '<option value="" disabled' + (prev ? '' : ' selected') + '>Kies een netwerk</option>';
      d.networks.forEach(function(n){
        if (!n.ssid || seen[n.ssid]) return;
        seen[n.ssid] = true;
        var opt = document.createElement('option');
        opt.value = n.ssid;
        opt.textContent = n.ssid + (n.secured ? ' · 🔒' : '');
        ssidEl.appendChild(opt);
      });
      if (prev && seen[prev]) ssidEl.value = prev;
      updateBtn();
    }).catch(function(){
      setTimeout(loadNetworks, 3000);
    });
  }

  function updateBtn() {
    var codeOk = codeEl.value.length === 6;
    btnEl.disabled = !codeOk || (!ethReady && !ssidEl.value);
  }

  codeEl.addEventListener('input', function(e){
    e.target.value = e.target.value.replace(/[^0-9]/g, '').slice(0, 6);
    updateBtn();
  });
  ssidEl.addEventListener('change', updateBtn);
  passEl.addEventListener('input', updateBtn);

  function setStatus(kind, title, msg) {
    statusEl.className = 'card status ' + kind;
    titleEl.textContent = title;
    msgEl.textContent = msg;
    var icon = statusEl.querySelector('.icon');
    if (kind === 'success') {
      icon.className = 'icon';
      icon.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M5 13l4 4L19 7"/></svg>';
    } else if (kind === 'error') {
      icon.className = 'icon';
      icon.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M6 6l12 12M18 6L6 18"/></svg>';
    } else {
      icon.className = 'icon spinner-wrap';
      icon.innerHTML = '<div class="spinner"></div>';
    }
  }

  btnEl.addEventListener('click', function(){
    var body = 'ssid=' + encodeURIComponent(ssidEl.value || '')
             + '&password=' + encodeURIComponent(passEl.value || '')
             + '&code=' + encodeURIComponent(codeEl.value);
    formEl.classList.add('hidden');
    statusEl.classList.remove('hidden');
    if (ethReady) {
      setStatus('info', 'Koppelen aan SlimHuys…', 'Code wordt gevalideerd.');
    } else {
      setStatus('info', 'Verbinden met WiFi…', 'Even geduld, dit duurt ongeveer 10 seconden.');
    }
    fetch('/save', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: body
    }).then(function(){
      setTimeout(pollStatus, 800);
    }).catch(function(){
      setStatus('error', 'Geen verbinding', 'Probeer de pagina te verversen en opnieuw.');
    });
  });

  function pollStatus() {
    fetch('/status').then(function(r){return r.json();}).then(function(d){
      if (d.status === 'connecting_wifi') {
        setStatus('info', 'Verbinden met WiFi…', 'Even geduld.');
        setTimeout(pollStatus, 1000);
      } else if (d.status === 'pairing') {
        setStatus('info', 'Koppelen aan SlimHuys…', 'Code wordt gevalideerd.');
        setTimeout(pollStatus, 1000);
      } else if (d.status === 'success') {
        setStatus('success', 'Klaar!', 'Bridge is gekoppeld. Je kunt deze pagina sluiten.');
      } else if (d.status === 'error') {
        setStatus('error', 'Oeps', d.message || 'Er ging iets mis.');
        setTimeout(function(){
          formEl.classList.remove('hidden');
          statusEl.classList.add('hidden');
        }, 4500);
      } else {
        setTimeout(pollStatus, 1000);
      }
    }).catch(function(){
      setTimeout(pollStatus, 2000);
    });
  }

  loadState();
})();
</script>
</body>
</html>
)=====";
