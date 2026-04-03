#!/bin/sh
# Package the built .app into an .ipa for sideloading.
set -e

BUILD_DIR="${1:-build/Build/Products/Release-iphoneos}"
APP_NAME="AlpineOnIOS"

if [ ! -d "$BUILD_DIR/$APP_NAME.app" ]; then
    echo "App not found at $BUILD_DIR/$APP_NAME.app"
    exit 1
fi

mkdir -p Payload
cp -r "$BUILD_DIR/$APP_NAME.app" Payload/
zip -r "$APP_NAME.ipa" Payload/
rm -rf Payload
echo "Created $APP_NAME.ipa"
