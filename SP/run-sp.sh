#!/usr/bin/env bash
# Run this fork's SP build as plain RTCW, straight from this repo (no mod, no outer project).
# Uses the binaries from `make` (build/release-<os>-<arch>/) and the game data (pak0 + sp_pak1-4) of an
# installed RTCW. Everything it creates lives under build/ (git-ignored): build/run/ (symlinks, fs_basepath)
# and build/home/ (fs_homepath: config, saves, logs), so your real ~/.wolf is never touched. It also builds the
# engine menus (SP/menus/) and loads them.
set -euo pipefail

SP="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(uname -m)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
BUILD="$SP/build/release-$OS-$ARCH"
RUN="$SP/build/run"
HOME_PATH="${RTCW_HOMEPATH:-$SP/build/home}"
WINDOWED=""
EXTRA_ARGS=()

usage() {
	cat <<'EOF'
Usage: SP/run-sp.sh [--debug] [--windowed WxH] [engine args...]

  --debug         Use build/debug-<os>-<arch> instead of the release build
  --windowed WxH  Windowed mode, e.g. 1280x720

Environment:
  RTCW_DATA      RTCW's Main folder with pak0.pk3 and sp_pak1-4.pk3 (Steam locations are auto-detected)
  RTCW_HOMEPATH  fs_homepath (default SP/build/home)
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--debug) BUILD="$SP/build/debug-$OS-$ARCH"; shift ;;
		--windowed) WINDOWED="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) EXTRA_ARGS+=("$1"); shift ;;
	esac
done

if [[ -z "${RTCW_DATA:-}" ]]; then
	for d in \
		"$HOME/.local/share/Steam/steamapps/common/Return to Castle Wolfenstein/Main" \
		"$HOME/.steam/steam/steamapps/common/Return to Castle Wolfenstein/Main" \
		"$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Return to Castle Wolfenstein/Main"; do
		if [[ -d "$d" ]]; then RTCW_DATA="$d"; break; fi
	done
fi
if [[ -z "${RTCW_DATA:-}" || ! -d "$RTCW_DATA" ]]; then
	echo "ERROR: RTCW data not found; set RTCW_DATA to the game's Main folder (pak0.pk3, sp_pak1-4.pk3)" >&2
	exit 1
fi

ENGINE="$BUILD/iowolfsp.$ARCH"
if [[ ! -x "$ENGINE" ]]; then
	echo "ERROR: $ENGINE not found; build first (make -C SP, or make -C SP debug for --debug)" >&2
	exit 1
fi

# Fresh symlink tree each run, so it always points at the current build and data.
mkdir -p "$RUN/main" "$HOME_PATH"
find "$RUN" -maxdepth 2 -type l -delete
ln -s "$ENGINE" "$RUN/iowolfsp.$ARCH"
for so in "$BUILD"/renderer_sp_*_"$ARCH".so; do ln -s "$so" "$RUN/"; done
for so in cgame qagame ui; do ln -s "$BUILD/main/$so.sp.$ARCH.so" "$RUN/main/"; done
for pak in pak0.pk3 sp_pak1.pk3 sp_pak2.pk3 sp_pak3.pk3 sp_pak4.pk3; do
	if [[ ! -f "$RTCW_DATA/$pak" ]]; then echo "ERROR: missing $RTCW_DATA/$pak" >&2; exit 1; fi
	ln -s "$RTCW_DATA/$pak" "$RUN/main/"
done

# Engine menus (rows for engine features, built from SP/menus/deltas + RTCW's own menus; see SP/menus/menudelta.py).
if command -v python3 >/dev/null && RTCW_DATA="$RTCW_DATA" python3 "$SP/menus/menudelta.py" build >/dev/null; then
	ln -s "$SP/menus/sp_zz_engine_menus.pk3" "$RUN/main/"
else
	echo "WARNING: engine menus not built (needs python3); running with RTCW's stock menus" >&2
fi

ARGS=(
	+set fs_basepath "$RUN"
	+set fs_homepath "$HOME_PATH"
	+set vm_game 0
	+set vm_cgame 0
	+set vm_ui 0
	+set sv_pure 0
	+set logfile 2
)
if [[ -n "$WINDOWED" ]]; then
	if [[ ! "$WINDOWED" =~ ^([0-9]+)x([0-9]+)$ ]]; then
		echo "ERROR: --windowed expects WxH, e.g. 1280x720 (got: $WINDOWED)" >&2
		exit 1
	fi
	ARGS+=(+set r_fullscreen 0 +set r_mode -1 +set r_customwidth "${BASH_REMATCH[1]}" +set r_customheight "${BASH_REMATCH[2]}")
fi

cd "$RUN"
exec "./iowolfsp.$ARCH" "${ARGS[@]}" "${EXTRA_ARGS[@]}"
