#!/usr/bin/env bash
# Publish PowerToysRunStandalone as a single-file self-contained win-x64 exe.
# Works on Linux/Windows with the .NET 8 SDK installed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$ROOT/src/PowerToysRunStandalone/PowerToysRunStandalone.csproj"
OUT="$ROOT/publish"

echo "==> dotnet publish (win-x64, self-contained, single-file)"
dotnet publish "$PROJECT" \
    -c Release \
    -r win-x64 \
    --self-contained true \
    -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true \
    -p:EnableCompressionInSingleFile=true \
    -p:DebugType=embedded \
    -o "$OUT"

echo "==> Published to $OUT"
find "$OUT" -maxdepth 1 -type f -printf '%f %s bytes\n'
