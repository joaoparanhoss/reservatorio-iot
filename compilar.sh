#!/bin/bash
# Compila um sketch localmente para a pasta build/.
# O arduino-cli exige que o .ino esteja numa pasta de mesmo nome,
# por isso a cópia para um diretório temporário.
#
#   ./compilar.sh                    # o sketch.ino versionado (broker placeholder)
#   ./compilar.sh sketch.ino.local   # a cópia com as credenciais reais do cluster
#   ./compilar.sh sketch.ino.publico # broker público, sem credencial nenhuma
#
# A variante é apenas a origem da cópia: o sketch.ino do repositório nunca é
# sobrescrito, então credencial de arquivo ignorado não escapa para o commit.
set -e
cd "$(dirname "$0")"

ORIGEM="${1:-sketch.ino}"
if [ ! -f "$ORIGEM" ]; then
  echo "erro: '$ORIGEM' não existe" >&2
  exit 1
fi

TMP=$(mktemp -d)
mkdir -p "$TMP/sketch"
cp "$ORIGEM" "$TMP/sketch/sketch.ino"
arduino-cli compile \
  --fqbn esp32:esp32:esp32doit-devkit-v1 \
  --output-dir "$PWD/build" \
  "$TMP/sketch"
rm -rf "$TMP"
echo "Firmware pronto em build/ (origem: $ORIGEM) — rode a simulação no Cursor (F1 > Wokwi: Start Simulator)"
