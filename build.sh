#!/usr/bin/env bash
# Builds GoogleDrive.dll from an MSYS2 UCRT64 shell (used by CI; build.cmd is the Windows equivalent).
#   ./build.sh            -> out/GoogleDrive/
#   ./build.sh package    -> also out/GoogleDrive-<version>-x64.zip
# Environment: BUILTIN_CLIENT_ID / BUILTIN_CLIENT_SECRET (optional, see src/builtin_client.h)
set -euo pipefail
cd "$(dirname "$0")"
OUT=out/GoogleDrive
mkdir -p "$OUT"

DEFS=()
if [[ -n "${BUILTIN_CLIENT_ID:-}" ]]; then
  DEFS+=("-DBUILTIN_CLIENT_ID=\"${BUILTIN_CLIENT_ID}\"" "-DBUILTIN_CLIENT_SECRET=\"${BUILTIN_CLIENT_SECRET:-}\"")
fi

g++ -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 "${DEFS[@]}" \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -shared -static -static-libgcc -static-libstdc++ \
    -I third_party \
    -o "$OUT/GoogleDrive.dll" src/GoogleDrive.cpp src/GoogleDrive.def \
    -lwinhttp -lws2_32 -lshell32 -lbcrypt -lcrypt32 -ladvapi32 -lole32 -s

cp GoogleDriveEng.lng GoogleDriveEng.hlf LICENSE "$OUT/"
cp CHANGELOG.md "$OUT/changelog.md"
echo "Built $OUT/GoogleDrive.dll"

if [[ "${1:-}" == "package" ]]; then
  VER=$(sed -n 's/.*PLUGIN_VERSION_STR *"\([^"]*\)".*/\1/p' src/version.h)
  ZIP="out/GoogleDrive-${VER}-x64.zip"
  rm -f "$ZIP"
  (cd out && zip -qr "GoogleDrive-${VER}-x64.zip" GoogleDrive)
  echo "Packaged $ZIP"
fi
