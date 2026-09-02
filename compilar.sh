#!/bin/bash
# Compila o sketch.ino localmente para a pasta build/.
# O arduino-cli exige que o .ino esteja numa pasta de mesmo nome,
# por isso a cópia para um diretório temporário.
set -e
cd "$(dirname "$0")"
TMP=$(mktemp -d)
mkdir -p "$TMP/sketch"
cp sketch.ino "$TMP/sketch/sketch.ino"
arduino-cli compile \
  --fqbn esp32:esp32:esp32doit-devkit-v1 \
  --output-dir "$PWD/build" \
  "$TMP/sketch"
rm -rf "$TMP"
echo "Firmware pronto em build/ — rode a simulação no Cursor (F1 > Wokwi: Start Simulator)"
