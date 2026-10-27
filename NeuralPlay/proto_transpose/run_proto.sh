#!/bin/bash
# Prototipo de transposicion local (Signalsmith). Convierte los stems originales de
# una cancion, transpone cada uno N semitonos con compensacion de formantes, los mezcla
# y genera audios para escuchar + mide el tiempo.
set -e
CLI="/Users/andresescobar/Neural/NeuralPlay/proto_transpose/signalsmith-stretch/cmd/stretch"
MIX="/Users/andresescobar/Neural/NeuralPlay/proto_transpose/signalsmith-stretch/cmd/mix"
SRC="$HOME/Library/Application Support/NeuralPlay/cache/song_14_t0"
SEMI="${1:-5}"
W=/tmp/proto
rm -rf "$W"; mkdir -p "$W/orig" "$W/tr"

echo "=== 1) convertir FLAC -> WAV ==="
n=0
for f in "$SRC"/*.flac; do
  b=$(basename "$f" .flac)
  afconvert "$f" "$W/orig/$b.wav" -d LEI16 -f WAVE 2>/dev/null
  n=$((n+1))
done
echo "convertidos: $n stems"

echo "=== 2) transponer cada stem +$SEMI (formant-comp) ==="
start=$(date +%s.%N)
for f in "$W/orig"/*.wav; do
  b=$(basename "$f" .wav)
  "$CLI" "$f" "$W/tr/$b.wav" --semitones=$SEMI --formant-comp >/dev/null 2>&1
done
end=$(date +%s.%N)
echo "tiempo transposicion de $n stems: $(echo "$end - $start" | bc) s"

# duracion del audio (para comparar contra tiempo de proceso)
DUR=$(afinfo "$W/orig/$(ls "$W/orig" | head -1)" 2>/dev/null | awk -F'[:,]' '/estimated duration/ {print $2}' | tr -d ' ')
echo "duracion de la cancion: ${DUR} s"

echo "=== 3) mezclar ==="
"$MIX" "$W/original_mix.wav" "$W/orig"/*.wav
"$MIX" "$W/transpuesto_mas${SEMI}_mix.wav" "$W/tr"/*.wav

echo "=== 4) comprimir a m4a para escuchar ==="
afconvert "$W/original_mix.wav" "$W/original_mix.m4a" -d aac -f m4af 2>/dev/null
afconvert "$W/transpuesto_mas${SEMI}_mix.wav" "$W/transpuesto_mas${SEMI}_mix.m4a" -d aac -f m4af 2>/dev/null
ls -la "$W"/*.m4a
echo "LISTO"
