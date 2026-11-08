#!/bin/sh
# One-shot build for the modernized EBView.
#
#   ./build.sh              release build in build/
#   ./build.sh --asan       AddressSanitizer build in build-asan/ (Debug)
#
# Any other argument is passed through to cmake, e.g. -DCMAKE_BUILD_TYPE=Debug.
set -eu

cd "$(dirname "$0")"

BUILD_DIR=build
BUILD_TYPE=Release
ASAN_FLAG=
ASAN=0
EXTRA_ARGS=

for arg in "$@"; do
	case "$arg" in
	--asan)
		BUILD_DIR=build-asan
		BUILD_TYPE=Debug
		ASAN_FLAG=-DENABLE_ASAN=ON
		ASAN=1
		;;
	*)
		# Word splitting on purpose: these are simple -D flags.
		EXTRA_ARGS="$EXTRA_ARGS $arg"
		;;
	esac
done

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" $ASAN_FLAG $EXTRA_ARGS
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "Build finished: $BUILD_DIR/ebview"
echo "Run without installing (data dir is auto-detected):"
echo "  ./$BUILD_DIR/ebview"
if [ "$ASAN" = 1 ]; then
	echo
	echo "AddressSanitizer build: 2-5x slower, reports memory errors with a"
	echo "stack trace. Leak reports from GTK's own one-shot allocations are"
	echo "noise, so they are off by default:"
	echo "  ASAN_OPTIONS=detect_leaks=0 ./$BUILD_DIR/ebview"
fi
echo "Or install system-wide:"
echo "  sudo cmake --install $BUILD_DIR"
