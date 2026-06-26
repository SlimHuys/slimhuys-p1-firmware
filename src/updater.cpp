#include "updater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "diaglog.h"

constexpr unsigned long CHECK_INTERVAL_MS       = 24UL * 3600UL * 1000UL;  // 24u na succes
constexpr unsigned long CHECK_RETRY_INTERVAL_MS =        5UL * 60UL * 1000UL;  // 5min na fout
constexpr unsigned long FIRST_CHECK_DELAY_MS    =              60UL * 1000UL;   // 60s na boot

static long parseVersion(const String& v) {
    // "1.23.4" → 1*1e6 + 23*1e3 + 4 = 1023004 — voor simpele < / > vergelijking
    long major = 0, minor = 0, patch = 0;
    int firstDot = v.indexOf('.');
    int secondDot = v.indexOf('.', firstDot + 1);
    if (firstDot < 0) return 0;
    major = v.substring(0, firstDot).toInt();
    if (secondDot < 0) {
        minor = v.substring(firstDot + 1).toInt();
    } else {
        minor = v.substring(firstDot + 1, secondDot).toInt();
        patch = v.substring(secondDot + 1).toInt();
    }
    return major * 1000000L + minor * 1000L + patch;
}

void OtaUpdater::begin(const String& baseUrl, const String& apiKey, const String& currentVersion) {
    _baseUrl = baseUrl;
    _apiKey = apiKey;
    _currentVersion = currentVersion;
    _lastCheckAt = 0;
    _firstCheckDone = false;
}

void OtaUpdater::loop() {
    unsigned long now = millis();

    if (!_firstCheckDone) {
        // Eerste check N seconden na boot (gun de bridge tijd om
        // ETH/WiFi/NTP/etc te stabiliseren).
        if (now > FIRST_CHECK_DELAY_MS) {
            _firstCheckDone = true;
            checkNow();
        }
        return;
    }

    // Na succes: elke 24u. Na een fout: retry na 5min.
    bool isError = (_lastResult != Result::UP_TO_DATE &&
                    _lastResult != Result::UPDATED_REBOOTING);
    unsigned long interval = isError ? CHECK_RETRY_INTERVAL_MS : CHECK_INTERVAL_MS;
    if (now - _lastCheckAt > interval) {
        checkNow();
    }
}

OtaUpdater::Result OtaUpdater::checkNow() {
    // _lastCheckAt pas zetten na een succesvolle check zodat een fout
    // na CHECK_RETRY_INTERVAL_MS (5min) opnieuw geprobeerd wordt i.p.v.
    // 24u te wachten.
    unsigned long checkStarted = millis();

    if (_baseUrl.isEmpty() || _apiKey.isEmpty()) {
        _lastCheckAt = checkStarted;
        _lastResult = Result::ERROR_NETWORK;
        return _lastResult;
    }

    WiFiClientSecure mc;
    mc.setInsecure();
    HTTPClient http;
    http.setTimeout(15000);
    http.begin(mc, _baseUrl + "/v1/firmware/manifest");
    http.addHeader("Authorization", "Bearer " + _apiKey);
    http.addHeader("X-Firmware-Version", _currentVersion);
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "slimhuys-p1/" + _currentVersion);

    int status = http.GET();

    if (status == 204) {
        // Geen update beschikbaar voor dit channel / deze bridge
        _availableVersion = "";
        _availableNotes = "";
        http.end();
        Serial.println("OTA: geen update beschikbaar");
        _lastCheckAt = checkStarted;
        _lastResult = Result::UP_TO_DATE;
        return _lastResult;
    }

    if (status != 200) {
        diagLog("OTA manifest faalde: HTTP %d", status);
        http.end();
        // Geen _lastCheckAt update — loop() retried na CHECK_RETRY_INTERVAL_MS
        _lastResult = Result::ERROR_NETWORK;
        return _lastResult;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        diagLog("OTA: manifest parse-fout");
        _lastResult = Result::ERROR_MANIFEST;
        return _lastResult;  // geen _lastCheckAt → retry na 5min
    }

    String targetVersion = doc["version"].as<String>();
    String url = doc["url"].as<String>();
    String sha256 = doc["sha256"].as<String>();
    _availableVersion = targetVersion;
    _availableNotes = doc["notes"].as<String>();
    _channel = doc["channel"].as<String>();

    if (targetVersion.isEmpty() || url.isEmpty() || sha256.isEmpty()) {
        diagLog("OTA: manifest mist verplichte velden");
        _lastResult = Result::ERROR_MANIFEST;
        return _lastResult;  // geen _lastCheckAt → retry na 5min
    }

    long current = parseVersion(_currentVersion);
    long target = parseVersion(targetVersion);
    if (target <= current) {
        Serial.printf("OTA: huidige versie %s is up-to-date (target %s)\n",
            _currentVersion.c_str(), targetVersion.c_str());
        _availableVersion = "";
        _availableNotes = "";
        _lastCheckAt = checkStarted;
        _lastResult = Result::UP_TO_DATE;
        return _lastResult;
    }

    Serial.printf("OTA: update beschikbaar %s → %s (channel %s)\n",
        _currentVersion.c_str(), targetVersion.c_str(), _channel.c_str());

    _lastCheckAt = checkStarted;  // telt als succesvolle check; download-fout → 5min retry
    _lastResult = _downloadAndFlash(url, sha256);
    return _lastResult;
}

OtaUpdater::Result OtaUpdater::_downloadAndFlash(const String& url, const String& expectedSha) {
    // GitHub Releases stuurt een 302 naar release-assets.githubusercontent.com.
    // HTTPC_FORCE_FOLLOW_REDIRECTS opent per hop een nieuwe verbinding —
    // geen TLS-sessie hergebruik — en werkt correct cross-domain.
    WiFiClientSecure dc;
    dc.setInsecure();
    HTTPClient http;
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.begin(dc, url);
    http.addHeader("User-Agent", "slimhuys-p1/" + _currentVersion);

    int status = http.GET();
    if (status != 200) {
        diagLog("OTA download faalde: HTTP %d", status);
        http.end();
        return Result::ERROR_DOWNLOAD;
    }

    int contentLen = http.getSize();
    if (contentLen <= 0) {
        diagLog("OTA: geen Content-Length, kan niet flashen");
        http.end();
        return Result::ERROR_DOWNLOAD;
    }

    diagLog("OTA: start flash %d bytes", contentLen);

    if (!Update.begin(contentLen)) {
        diagLog("OTA Update.begin faalde: %s", Update.errorString());
        http.end();
        return Result::ERROR_FLASH;
    }

    // Stream + parallelle SHA-256-berekening
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t totalRead = 0;
    unsigned long lastDataAt = millis();

    while (totalRead < (size_t)contentLen) {
        size_t available = stream->available();
        if (available > 0) {
            size_t toRead = available > sizeof(buf) ? sizeof(buf) : available;
            int n = stream->readBytes(buf, toRead);
            if (n > 0) {
                if (Update.write(buf, n) != (size_t)n) {
                    diagLog("OTA Update.write faalde: %s", Update.errorString());
                    mbedtls_sha256_free(&ctx);
                    Update.abort();
                    http.end();
                    return Result::ERROR_FLASH;
                }
                mbedtls_sha256_update(&ctx, buf, n);
                totalRead += n;
                lastDataAt = millis();

                // Progress-log per ~10%
                static int lastPct = -1;
                int pct = (totalRead * 100) / contentLen;
                if (pct / 10 != lastPct / 10) {
                    Serial.printf("OTA: %d%% (%u/%d)\n", pct, totalRead, contentLen);
                    lastPct = pct;
                }
            }
        } else {
            delay(1);
            // Timeout op basis van laatste ontvangen data, niet totale tijd —
            // http.connected() is onbetrouwbaar bij TLS en breekt te vroeg af.
            if (millis() - lastDataAt > 30000) {
                diagLog("OTA: download timeout (geen data na 30s)");
                mbedtls_sha256_free(&ctx);
                Update.abort();
                http.end();
                return Result::ERROR_DOWNLOAD;
            }
        }
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hashHex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hashHex + i * 2, "%02x", hash[i]);
    }

    String expectedLower = expectedSha;
    expectedLower.toLowerCase();
    if (String(hashHex) != expectedLower) {
        diagLog("OTA SHA-256 mismatch: got %s", hashHex);
        Update.abort();
        http.end();
        return Result::ERROR_HASH;
    }

    if (!Update.end(true)) {
        diagLog("OTA Update.end faalde: %s", Update.errorString());
        http.end();
        return Result::ERROR_FLASH;
    }

    http.end();
    diagLog("OTA: flash klaar — reboot");
    delay(1500);
    ESP.restart();
    return Result::UPDATED_REBOOTING;
}

void OtaUpdater::markValid() {
    if (_validMarked) return;
    _validMarked = true;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        Serial.println("OTA: huidige firmware gemarkeerd als valid (rollback geannuleerd)");
    }
    // Niet-OK is meestal "geen pending OTA" — geen probleem, gewoon negeren.
}

long OtaUpdater::lastCheckAtMsAgo() const {
    return _lastCheckAt > 0 ? (long)(millis() - _lastCheckAt) : -1;
}
