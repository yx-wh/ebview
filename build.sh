#!/bin/sh
# One-shot build for the modernized EBView.
set -eu

cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build --parallel "$(nproc)"

echo
echo "Build finished: build/ebview"
echo "Run without installing:"
echo "  EBVIEW_DATADIR=\"$PWD/build/share/ebview\" ./build/ebview"
echo "Or install system-wide:"
echo "  sudo cmake --install build"
