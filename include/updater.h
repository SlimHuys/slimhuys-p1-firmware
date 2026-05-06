/**
 * SlimHuys OTA-updater
 * --------------------
 * Periodiek check naar /v1/firmware/manifest, vergelijkt versie, downloadt
 * + verifieert SHA-256 + flasht via Update.h + reboot.
 *
 * Backend-contract:
 *   GET <base_url>/v1/firmware/manifest
 *   Headers: Authorization: Bearer <api_key>
 *            X-Firmware-Version: <current>
 *   Response (200): { "version", "url", "sha256", "notes", "channel" }
 *   Response (204): geen update beschikbaar voor deze bridge / dit channel
 *
 * Roll-back: na succesvolle boot moet main-code expliciet
 * markValid() aanroepen (bijv. na eerste API-push), anders rollback
 * de bootloader bij volgende reboot naar de vorige firmware.
 */
#pragma once
#include <Arduino.h>

class OtaUpdater {
public:
    enum class Result {
        UP_TO_DATE,
        UPDATED_REBOOTING,    // geslaagd → bord reboot meteen
        ERROR_NETWORK,
        ERROR_MANIFEST,
        ERROR_DOWNLOAD,
        ERROR_HASH,
        ERROR_FLASH,
    };

    void begin(const String& baseUrl, const String& apiKey, const String& currentVersion);
    void loop();

    // Trigger een check meteen (handmatig vanuit management-UI). Blokkeert.
    Result checkNow();

    // Markeer huidige firmware als "werkend" zodat bootloader niet rollback'd.
    // Aan te roepen door main-code na bewezen success (bv. eerste API-push).
    void markValid();

    // Status-getters voor management-UI
    String availableVersion() const { return _availableVersion; }
    String availableNotes() const { return _availableNotes; }
    String channel() const { return _channel; }
    long lastCheckAtMsAgo() const;
    Result lastResult() const { return _lastResult; }

private:
    String _baseUrl;
    String _apiKey;
    String _currentVersion;

    unsigned long _lastCheckAt = 0;
    bool _firstCheckDone = false;

    String _availableVersion;
    String _availableNotes;
    String _channel;
    Result _lastResult = Result::UP_TO_DATE;

    bool _validMarked = false;

    Result _downloadAndFlash(const String& url, const String& expectedSha);
};
