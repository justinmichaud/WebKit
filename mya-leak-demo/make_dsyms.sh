#!/bin/bash
# Generate dSYMs for the WebKit images that the Debug build does not already write
# one for (it writes JavaScriptCore's and WebCore's). Reads Xcode's compilation
# cache with -cas so the precompiled headers' types are included; without that,
# liblldb can crash on what is left. From design.md "Build and running".
set -u
cd /Users/justinmichaud/Development/WebKit/WebKitBuild/Debug || exit 1
CAS=~/Library/Developer/Xcode/DerivedData/CompilationCache.noindex/builtin
for image in WebKit WebKitLegacy WebGPU _WebKit_SwiftUI; do
  fw="$image.framework/Versions/A/$image"
  [ -f "$fw" ] || { echo "skip (missing): $fw"; continue; }
  echo "dsym: $image.framework"
  xcrun dsymutil -cas "$CAS" "$fw" -o "$image.framework.dSYM" 2>&1 | tail -2
done
for image in libANGLE-shared.dylib libwebrtc.dylib libWebKitSwift.dylib; do
  [ -f "$image" ] || { echo "skip (missing): $image"; continue; }
  echo "dsym: $image"
  xcrun dsymutil -cas "$CAS" "$image" -o "$image.dSYM" 2>&1 | tail -2
done
echo "done. dSYMs present:"
ls -d *.dSYM 2>/dev/null
