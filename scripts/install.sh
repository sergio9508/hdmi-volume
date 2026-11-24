#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

make all

echo "Installing HDMIVolume.driver into /Library/Audio/Plug-Ins/HAL (requires sudo)..."
sudo rm -rf /Library/Audio/Plug-Ins/HAL/HDMIVolume.driver
sudo cp -R build/HDMIVolume.driver /Library/Audio/Plug-Ins/HAL/HDMIVolume.driver
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/HDMIVolume.driver
sudo chmod -R 755 /Library/Audio/Plug-Ins/HAL/HDMIVolume.driver

echo "Restarting coreaudiod..."
sudo killall coreaudiod || true

echo "Installing HDMIVolume.app into /Applications..."
rm -rf /Applications/HDMIVolume.app
cp -R build/HDMIVolume.app /Applications/HDMIVolume.app

echo "Done! Launch HDMIVolume from /Applications/HDMIVolume.app"
