#!/bin/zsh
cd -- "${0:A:h}" || exit 1
printf 'Starting simulated flowmeter.\nIn Foxglove: Open connection > Foxglove WebSocket > ws://localhost:5555\nPress Control-C here to stop and close the recording.\n\n'
exec ./bin/mini-drivebrain-macos-arm64 --simulate --hz 20 --recordings ./viewer-verification/recordings
