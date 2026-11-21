#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# gmsl-capture.sh - capture frames from a MAX96724 GMSL2 camera
#
# Usage: gmsl-capture.sh [options]
#   -l, --link N       GMSL link: 0=A, 1=B, 2=C, 3=D (default: auto)
#   -n, --frames N     number of frames to capture (default: 60)
#   -o, --output FILE  raw UYVY output (default: $HOME/gmsl-capture.raw)
#   -p, --png FILE     last-frame PNG output (default: $HOME/gmsl-capture.png)
#   -s, --size WxH     capture size (default: 1920x1080; AR0820: 3840x2160)
#   -r, --rotate       rotate the PNG 180 degrees
#   -h, --help         show this help

set -u

SIZE=1920x1080
WIDTH=1920
HEIGHT=1080
FORMAT=UYVY
MBUS=UYVY8_1X16
LINK=auto
FRAMES=60
OUT="$HOME/gmsl-capture.raw"
PNG="$HOME/gmsl-capture.png"
ROTATE=0

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
	-l|--link) LINK=$2; shift 2 ;;
	-n|--frames) FRAMES=$2; shift 2 ;;
	-o|--output) OUT=$2; shift 2 ;;
	-p|--png) PNG=$2; shift 2 ;;
	-s|--size) SIZE=$2; shift 2 ;;
	-r|--rotate) ROTATE=1; shift ;;
	-h|--help) sed -n '2,13p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
done

case "$SIZE" in
*x*) WIDTH=${SIZE%x*}; HEIGHT=${SIZE#*x} ;;
*) die "invalid size $SIZE (use WxH)" ;;
esac
case "$WIDTH$HEIGHT" in
*[!0-9]*|'') die "invalid size $SIZE" ;;
esac
[ "$WIDTH" -gt 0 ] && [ "$HEIGHT" -gt 0 ] || die "invalid size $SIZE"

for cmd in v4l2-ctl media-ctl; do
	command -v "$cmd" >/dev/null 2>&1 ||
		die "$cmd not found (install v4l-utils)"
done

# Find the gmsl_links attribute and auto-detect the active link.
LINKS_FILE=
for f in /sys/bus/i2c/devices/*/gmsl_links; do
	[ -e "$f" ] && LINKS_FILE=$f && break
done
[ -n "$LINKS_FILE" ] || die "gmsl_links not found (is max96724_imx390 loaded?)"

if [ "$LINK" = auto ]; then
	read -r A B C D < "$LINKS_FILE" || :
	[ -n "${A:-}" ] && [ -n "${B:-}" ] && [ -n "${C:-}" ] &&
		[ -n "${D:-}" ] || die "cannot parse $LINKS_FILE"
	case "$A$B$C$D" in
	1000) LINK=0 ;;
	0100) LINK=1 ;;
	0010) LINK=2 ;;
	0001) LINK=3 ;;
	*) die "no active GMSL link (gmsl_links=$A $B $C $D)" ;;
	esac
fi

case "$LINK" in
0|1|2|3) ;;
*) die "invalid link $LINK (0=A, 1=B, 2=C, 3=D)" ;;
esac

# Find the rp1-cfe media device (numbering can change between boots).
MEDIA=
for m in /dev/media*; do
	if media-ctl -p -d "$m" 2>/dev/null | grep -q 'driver *rp1-cfe'; then
		MEDIA=$m
		break
	fi
done
[ -n "$MEDIA" ] || die "rp1-cfe media device not found"

# Find the video node for this CSI2 channel.
CH="rp1-cfe-csi2_ch$LINK"
VIDEO=$(media-ctl -p -d "$MEDIA" | awk -v ch="$CH" '
	$0 ~ "^- entity .*: " ch " " { found = 1; next }
	found && /device node name/ { print $NF; exit }
')
[ -n "$VIDEO" ] || die "video node for $CH not found"

SRC_PAD=$((4 + LINK))
echo "link:  $LINK"
echo "media: $MEDIA"
echo "video: $VIDEO"

# Disable every CSI2 channel first, then enable only the requested one.
# Otherwise CFE keeps the previous channel enabled-but-not-streaming and
# refuses to schedule the new channel's first job.
for p in 0 1 2 3; do
	media-ctl -d "$MEDIA" \
		-l "\"csi2\":$((4 + p))->\"rp1-cfe-csi2_ch$p\":0[0]" \
		2>/dev/null || true
done

# Enable the active CSI2 channel and make all four sink pads match the sensor.
media-ctl -d "$MEDIA" -l "\"csi2\":$SRC_PAD->\"$CH\":0[1]" || exit 1
for p in 0 1 2 3; do
	media-ctl -d "$MEDIA" \
		-V "\"csi2\":$p[fmt:$MBUS/${WIDTH}x${HEIGHT} field:none]" ||
		exit 1
done

v4l2-ctl -d "$VIDEO" \
	--set-fmt-video=width=$WIDTH,height=$HEIGHT,pixelformat=$FORMAT || exit 1

rm -f "$OUT"
v4l2-ctl -d "$VIDEO" --stream-mmap --stream-count="$FRAMES" \
	--stream-to="$OUT" || die "capture failed"
[ -s "$OUT" ] || die "capture produced no data"

FRAME_SIZE=$((WIDTH * HEIGHT * 2))
ACTUAL_FRAMES=$(( $(stat -c %s "$OUT") / FRAME_SIZE ))
echo "captured: $ACTUAL_FRAMES frames -> $OUT ($(stat -c %s "$OUT") bytes)"

# Last frame statistics and optional PNG.
if command -v python3 >/dev/null 2>&1; then
	python3 - "$OUT" "$FRAME_SIZE" "$ACTUAL_FRAMES" <<'PY'
import sys
path, frame_size, frames = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, 'rb') as f:
    f.seek((frames - 1) * frame_size)
    data = f.read(frame_size)
y, u, v = data[1::2], data[0::4], data[2::4]
for name, plane in (('Y', y), ('U', u), ('V', v)):
    print('%s min=%d max=%d mean=%.2f' %
          (name, min(plane), max(plane), sum(plane) / len(plane)))
PY
fi

if command -v ffmpeg >/dev/null 2>&1 && [ "$ACTUAL_FRAMES" -gt 0 ]; then
	dd if="$OUT" of="$OUT.last" bs=$FRAME_SIZE \
		skip=$((ACTUAL_FRAMES - 1)) count=1 status=none
	if [ "$ROTATE" -eq 1 ]; then
		ffmpeg -hide_banner -loglevel error -y -f rawvideo \
			-pix_fmt uyvy422 -s ${WIDTH}x${HEIGHT} -i "$OUT.last" \
			-vf hflip,vflip -frames:v 1 "$PNG"
	else
		ffmpeg -hide_banner -loglevel error -y -f rawvideo \
			-pix_fmt uyvy422 -s ${WIDTH}x${HEIGHT} -i "$OUT.last" \
			-frames:v 1 "$PNG"
	fi
	ret=$?
	rm -f "$OUT.last"
	[ $ret -eq 0 ] && echo "png: $PNG$([ "$ROTATE" -eq 1 ] && echo ' (rotated 180)')"
fi
