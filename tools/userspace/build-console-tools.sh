#!/bin/sh
# Build the seed's static tools. The assembly entry saves the initial SP.
# Usage: sh build-console-tools.sh output-directory
set -eu
SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT=${1:?usage: build-console-tools.sh output-directory}
mkdir -p "$OUTPUT"
for name in ls cat; do
    xcrun --sdk iphoneos clang -target arm64-apple-ios15.0 \
        -fno-stack-protector -fno-builtin -nostdlib -Wl,-static \
        -Wl,-e,__start -o "$OUTPUT/$name" "$SOURCE/start.S" "$SOURCE/$name.c"
    codesign -f -s - "$OUTPUT/$name"
done
