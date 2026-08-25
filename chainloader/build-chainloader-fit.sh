#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UBOOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX_SHIM="$SCRIPT_DIR/chainloader-prefix-shim.uImage"
SHIM="$SCRIPT_DIR/chainloader-shim.bin"
BOARD="${5:-xg2010g}"
DTB="$SCRIPT_DIR/$BOARD-chainloader-control.dtb"
TEMPLATE="$SCRIPT_DIR/$BOARD-chainloader.its.in"

PAYLOAD="$(realpath "${1:-$UBOOT_DIR/u-boot.bin}")"
OUTPUT_DIR="$(realpath -m "${2:-$UBOOT_DIR/out}")"
MKIMAGE="${3:-$UBOOT_DIR/tools/mkimage}"
DUMPIMAGE="${4:-$UBOOT_DIR/tools/dumpimage}"
FDTGET="${FDTGET:-fdtget}"

if [ ! -f "$PAYLOAD" ]; then
    echo "Error: payload not found: $PAYLOAD" >&2
    exit 1
fi

if [ ! -f "$MKIMAGE" ]; then
    echo "Error: mkimage not found: $MKIMAGE" >&2
    exit 1
fi

if ! command -v "$FDTGET" >/dev/null 2>&1; then
    echo "Error: fdtget not found: $FDTGET" >&2
    exit 1
fi

if [ ! -f "$SHIM" ]; then
    echo "Error: chainloader shim not found: $SHIM" >&2
    exit 1
fi

if [ ! -f "$DTB" ]; then
    echo "Error: control DTB not found for board '$BOARD': $DTB" >&2
    exit 1
fi

if [ ! -f "$TEMPLATE" ]; then
    echo "Error: ITS template not found for board '$BOARD': $TEMPLATE" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Keep all artifacts in one stable directory while making each build easy to
# identify.  XG2010G_BUILD_STAMP can be supplied by CI for reproducibility.
BUILD_STAMP="${XG2010G_BUILD_STAMP:-$(date +%Y%m%d-%H%M%S)}"
EXPECTED_COMMIT="$(git -C "$UBOOT_DIR" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
PAYLOAD_VERSION="$(strings "$PAYLOAD" 2>/dev/null | sed -n 's/.*U-Boot \(XG2010G-recovery-[^ ]*\).*/\1/p' | head -1 || true)"
if [ -n "$PAYLOAD_VERSION" ] && [ "$EXPECTED_COMMIT" != "unknown" ] &&
   [ "${ALLOW_PAYLOAD_VERSION_MISMATCH:-0}" != "1" ] &&
   [[ "$PAYLOAD_VERSION" != *"-g${EXPECTED_COMMIT}"* ]]; then
  echo "Error: payload version '$PAYLOAD_VERSION' does not match HEAD '$EXPECTED_COMMIT'. Rebuild U-Boot before packaging." >&2
  exit 1
fi
COMMIT_ID="${XG2010G_COMMIT_ID:-$EXPECTED_COMMIT}"
OUTPUT_PREFIX="$BOARD-chainloader-${BUILD_STAMP}-g${COMMIT_ID}"
OUTPUT_FIT="$OUTPUT_DIR/${OUTPUT_PREFIX}.itb"
OUTPUT_SLOT="$OUTPUT_DIR/${OUTPUT_PREFIX}-slot.bin"
OUTPUT_PAYLOAD="$OUTPUT_DIR/${BOARD}-u-boot-${BUILD_STAMP}-g${COMMIT_ID}.bin"
LATEST_FIT="$OUTPUT_DIR/$BOARD-chainloader.itb"
LATEST_SLOT="$OUTPUT_DIR/$BOARD-chainloader-slot.bin"
MAX_SLOT_SIZE=$((0x200000))
MIN_PAYLOAD_SIZE=$((900 * 1024))
FIT_OFFSET=$((0x2100))

payload_size=$(wc -c < "$PAYLOAD")
shim_size=$(wc -c < "$SHIM")
if [ "$payload_size" -lt "$MIN_PAYLOAD_SIZE" ]; then
  echo "Error: payload is too small for a full secondary U-Boot: $payload_size bytes" >&2
  echo "Refusing to package a shim-sized chainloader payload." >&2
  exit 1
fi

control_fit_base=$("$FDTGET" -tx "$DTB" / fit-base)
printf -v expected_fit_base '%x' $((0x81800000 + FIT_OFFSET))
if [ "$control_fit_base" != "$expected_fit_base" ]; then
  echo "Error: control DTB fit-base is 0x$control_fit_base; expected 0x$expected_fit_base" >&2
  exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

ITS="$TMPDIR/$BOARD-chainloader.its"

sed \
  -e "s|__DTB__|$DTB|g" \
  -e "s|__SHIM__|$SHIM|g" \
  -e "s|__PAYLOAD__|$PAYLOAD|g" \
  -e "s|__KCOMP__|none|g" \
  "$TEMPLATE" > "$ITS"

echo "Building FIT image..."
"$MKIMAGE" -f "$ITS" "$OUTPUT_FIT"
"$DUMPIMAGE" -l "$OUTPUT_FIT"

FIT_SUMMARY="$TMPDIR/$BOARD-chainloader-summary.txt"
"$DUMPIMAGE" -l "$OUTPUT_FIT" > "$FIT_SUMMARY"
fit_image_field() {
  local image="$1"
  local field="$2"

  awk -v image="$image" -v field="$field" '
    $1 == "Image" && $3 == "(" image ")" { in_image = 1; next }
    in_image && $1 == field { print $3; exit }
  ' "$FIT_SUMMARY"
}

fit_shim_size=$(fit_image_field kernel@1 "Data")
fit_payload_size=$(fit_image_field uboot@1 "Data")
fit_shim_load=$("$FDTGET" -tx "$OUTPUT_FIT" '/images/kernel@1' load)
fit_shim_entry=$("$FDTGET" -tx "$OUTPUT_FIT" '/images/kernel@1' entry)
fit_payload_load=$("$FDTGET" -tx "$OUTPUT_FIT" '/images/uboot@1' load)
fit_payload_entry=$("$FDTGET" -tx "$OUTPUT_FIT" '/images/uboot@1' entry)

if [ "$fit_shim_size" != "$shim_size" ]; then
  echo "Error: FIT kernel@1 size ($fit_shim_size) does not match shim ($shim_size)" >&2
  exit 1
fi
if [ "$fit_shim_load" != "80288000" ] ||
   [ "$fit_shim_entry" != "80288000" ]; then
  echo "Error: FIT kernel@1 shim load/entry is not 0x80288000" >&2
  exit 1
fi
if [ "$fit_payload_size" != "$payload_size" ]; then
  echo "Error: FIT uboot@1 size ($fit_payload_size) does not match payload ($payload_size)" >&2
  exit 1
fi
if [ "$fit_payload_load" != "81e00000" ] ||
   [ "$fit_payload_entry" != "81e00000" ]; then
  echo "Error: FIT uboot@1 load/entry is not 0x81e00000" >&2
  exit 1
fi

echo "Building slot image..."
prefix_size=$(wc -c < "$PREFIX_SHIM")
if [ "$prefix_size" -gt "$FIT_OFFSET" ]; then
  echo "Error: chainloader prefix exceeds FIT offset: $prefix_size bytes" >&2
  exit 1
fi
cp "$PREFIX_SHIM" "$OUTPUT_SLOT"
dd if=/dev/zero bs=1 count=$((FIT_OFFSET - prefix_size)) >> "$OUTPUT_SLOT" 2>/dev/null
cat "$OUTPUT_FIT" >> "$OUTPUT_SLOT"

# Stable aliases are retained for existing tooling; the timestamped files
# above are the canonical artifacts to archive or flash.
cp "$PAYLOAD" "$OUTPUT_PAYLOAD"
cp "$OUTPUT_FIT" "$LATEST_FIT"
cp "$OUTPUT_SLOT" "$LATEST_SLOT"

slot_size=$(wc -c < "$OUTPUT_SLOT")
if [ "$slot_size" -ge "$MAX_SLOT_SIZE" ]; then
  echo "Error: chainloader slot is not smaller than 2 MiB: $slot_size bytes" >&2
  exit 1
fi
echo ""
echo "Done!"
echo "  FIT:  $OUTPUT_FIT ($(wc -c < "$OUTPUT_FIT") bytes)"
echo "  Slot: $OUTPUT_SLOT ($slot_size bytes; maximum $((MAX_SLOT_SIZE - 1)) bytes)"
echo "  U-Boot payload: $OUTPUT_PAYLOAD ($payload_size bytes)"
echo "  Latest aliases: $LATEST_FIT, $LATEST_SLOT"
echo ""
echo "Magic check:"
echo "  Offset 0x0000: $(dd if="$OUTPUT_SLOT" bs=1 count=4 2>/dev/null | od -A n -t x1 | tr -d ' \n')"
echo "  Offset 0x2100: $(dd if="$OUTPUT_SLOT" bs=1 skip=$((0x2100)) count=4 2>/dev/null | od -A n -t x1 | tr -d ' \n')"
