#!/usr/bin/env bash
# Lokale release-bouw: produceert release/ met merged.bin, ota-bin en
# manifest.json voor ESP Web Tools. CI doet hetzelfde maar vervangt het
# manifest-pad door de GitHub Release-URL — zie .github/workflows/release.yml.
#
# Gebruik:  scripts/release.sh
# Output:   release/<version>/  met de 3 artefacten

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION=$(grep FIRMWARE_VERSION platformio.ini | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [[ -z "$VERSION" ]]; then
  echo "FIRMWARE_VERSION niet gevonden in platformio.ini" >&2
  exit 1
fi

echo "→ Building firmware v$VERSION (V3 + V4)"
pio run -e smarthomeshop-v3
pio run -e smarthomeshop-v4

OUT="release/$VERSION"
mkdir -p "$OUT/v3" "$OUT/v4"

# V3 (ESP32 + LAN8720)
cp .pio/build/smarthomeshop-v3/firmware.bin        "$OUT/v3/firmware.bin"
cp .pio/build/smarthomeshop-v3/firmware-merged.bin "$OUT/v3/firmware-merged.bin"
shasum -a 256 "$OUT/v3/firmware.bin" | awk '{print $1}' > "$OUT/v3/firmware.bin.sha256"

# V4 (ESP32-C6 + W5500)
cp .pio/build/smarthomeshop-v4/firmware.bin        "$OUT/v4/firmware.bin"
cp .pio/build/smarthomeshop-v4/firmware-merged.bin "$OUT/v4/firmware-merged.bin"
shasum -a 256 "$OUT/v4/firmware.bin" | awk '{print $1}' > "$OUT/v4/firmware.bin.sha256"

cat > "$OUT/manifest.json" <<EOF
{
  "name": "SlimHuys P1-bridge",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "v3/firmware-merged.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C6",
      "parts": [
        { "path": "v4/firmware-merged.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

echo
echo "✓ Release-artefacten in $OUT/"
ls -lh "$OUT/v3/" "$OUT/v4/"
echo
echo "Lokaal testen (python http-server):"
echo "  cd $OUT && python3 -m http.server 8000"
echo "  → open https://esphome.github.io/esp-web-tools/ en plak: http://localhost:8000/manifest.json"
