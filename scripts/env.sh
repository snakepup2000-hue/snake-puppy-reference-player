#!/bin/bash
# Shared build environment for Snake Puppy Reference Player.
# This machine has an incomplete CommandLineTools SDK; Xcode's SDK works,
# so export SDKROOT when needed.
if [ -z "${SDKROOT:-}" ] || [ ! -d "${SDKROOT:-/nonexistent}" ]; then
  CANDIDATE=$(ls -d /Applications/Xcode*.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk 2>/dev/null | sort -V | tail -1)
  if [ -n "$CANDIDATE" ]; then
    export SDKROOT="$CANDIDATE"
  fi
fi
