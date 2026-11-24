#!/usr/bin/env bash
set -euo pipefail

pkill -x HDMIVolume || true
rm -rf /Applications/HDMIVolume.app

echo "Removing HDMIVolume.driver from /Library/Audio/Plug-Ins/HAL (requires sudo)..."
sudo rm -rf /Library/Audio/Plug-Ins/HAL/HDMIVolume.driver
sudo killall coreaudiod || true

echo "Uninstalled HDMIVolume."
