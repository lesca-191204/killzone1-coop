#!/bin/sh
# Arma el paquete del co-op de KZ1: launcher (PyInstaller, sin Python en la PC del jugador) + juego + plantilla de memory card.
# Uso (Git Bash): sh launcher/package.sh   ->  kz1_work/release/KZ1 Coop/  y  kz1_work/release/KZ1-Coop-v<ver>.zip
set -e
W=/c/Users/Santi/Documents/Port/kz1_work
PY=${PY:-/c/Users/Santi/Documents/Port/kzsf_pc/out/pyenv/Scripts/python.exe}   # venv con pyinstaller + pywebview
BUILD=$W/KillzoneRecomp/build/RelWithDebInfo
VER=$(grep -o "^VERSION = '[^']*'" $W/launcher/kz1_launcher.py | cut -d"'" -f2)
OUT=$W/release
NAME="KZ1 Coop"
DEST="$OUT/$NAME"

mkdir -p "$OUT/pyi"
"$PY" -m PyInstaller --noconfirm --clean --log-level WARN --windowed --name "$NAME" \
    --icon "$(cygpath -w $W/launcher/assets/kz1.ico)" --distpath "$(cygpath -w $OUT/pyi/dist)" \
    --workpath "$(cygpath -w $OUT/pyi/work)" --specpath "$(cygpath -w $OUT/pyi/work)" \
    --add-data "$(cygpath -w $W/launcher/web);web" "$(cygpath -w $W/launcher/kz1_launcher.py)"

# Carpeta limpia para el zip (sin ISO, ajustes ni partidas de nadie).
STAGE="$OUT/stage/$NAME"
rm -rf "$OUT/stage"
mkdir -p "$STAGE/game" "$STAGE/coop" "$STAGE/launcher"
cp -r "$OUT/pyi/dist/$NAME/." "$STAGE/"
cp "$BUILD/killzone.exe" "$BUILD"/*.dll "$STAGE/game/"
cp -r "$BUILD/resources" "$STAGE/game/"
cp -r "$W/coop/mc_template" "$STAGE/coop/"
cp "$W/launcher/assets/art.jpg" "$W/launcher/assets/kz1.ico" "$W/launcher/assets/kz1.png" "$STAGE/launcher/"
cp "$W/launcher/LEEME.txt" "$STAGE/"
# Ajustes de fabrica del juego: los del ini de desarrollo, sin la ISO ni el launcher propio del exe.
sed -e 's/^ISO=.*/ISO=/' -e 's/^Show=.*/Show=0/' -e 's/^ShowFps=.*/ShowFps=0/' -e 's/^FieldOfView=.*/FieldOfView=90/'     -e 's/^RenderDistance=.*/RenderDistance=1/' -e 's/^Upscale=.*/Upscale=4/' -e 's/^SkipIntro=.*/SkipIntro=0/'     "$BUILD/killzone.ini" > "$STAGE/game/killzone.ini"
cd "$OUT"
rm -f "KZ1-Coop-v$VER.zip"
powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$STAGE")' -DestinationPath '$(cygpath -w "$OUT/KZ1-Coop-v$VER.zip")' -CompressionLevel Optimal"
cd "$W"
# (si el launcher o el juego estan abiertos, sus archivos no se pueden reemplazar: cerrarlos y volver a correr)
set +e
# La carpeta de uso (la de Santi): se actualizan los programas; ISO, launcher.json, killzone.ini, partidas y logs quedan.
mkdir -p "$DEST"
cp -r "$OUT/pyi/dist/$NAME/." "$DEST/"
mkdir -p "$DEST/game" "$DEST/coop" "$DEST/launcher"
cp "$STAGE/game/killzone.exe" "$STAGE/game/"*.dll "$DEST/game/"
cp -r "$STAGE/game/resources" "$DEST/game/"
cp -r "$STAGE/coop/mc_template" "$DEST/coop/"
cp "$STAGE/launcher/"* "$DEST/launcher/"
cp "$STAGE/LEEME.txt" "$DEST/"
[ -f "$DEST/game/killzone.ini" ] || cp "$STAGE/game/killzone.ini" "$DEST/game/"
ls -la "$OUT"
