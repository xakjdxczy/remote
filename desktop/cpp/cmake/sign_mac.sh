#!/bin/zsh
set -e
APP="$1"
CAM_ENT="$2"
APP_ENT="$3"
EXT="$APP/Contents/Library/SystemExtensions/com.dustx.remotedesk.camera.systemextension"
# Open-source default: ad-hoc. Do not pick up a company Developer ID from the keychain.
# Official release: DUSTX_SIGN_IDENTITY='Developer ID Application: …'
ID="${DUSTX_SIGN_IDENTITY:--}"

sign_one() {
  if [[ "$ID" != "-" ]]; then
    codesign --force --options runtime --timestamp --sign "$ID" "$@"
  else
    codesign --force --sign - "$@"
  fi
}

if [[ -d "$APP/Contents/Resources/DustXMic.driver" ]]; then
  sign_one "$APP/Contents/Resources/DustXMic.driver"
fi
if [[ -d "$APP/Contents/Resources/DustXCam.plugin" ]]; then
  sign_one "$APP/Contents/Resources/DustXCam.plugin"
fi
if [[ -x "$APP/Contents/Resources/platform-tools/adb" ]]; then
  sign_one "$APP/Contents/Resources/platform-tools/adb"
fi
sign_one --entitlements "$CAM_ENT" "$EXT"
sign_one --entitlements "$APP_ENT" "$APP"
echo "signed with: $ID"
