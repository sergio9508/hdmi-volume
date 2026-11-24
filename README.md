# HDMI Volume

Minimalist macOS CoreAudio HAL loopback driver + menu bar companion app to control HDMI/DisplayPort audio volume using the Mac keyboard's native volume keys.

## Architecture

- **`Driver/` (`HDMIVolume.driver`)**: C `AudioServerPlugIn` that registers a virtual `HDMI Volume` device with writable native volume (`kAudioLevelControlClassID`) and mute (`kAudioMuteControlClassID`) controls, scaling output audio in real time into a loopback input stream.
- **`App/` (`HDMIVolume.app`)**: Swift/AppKit menu bar app (`LSUIElement`) that bridges audio from `HDMI Volume` to your HDMI/DisplayPort display (`AudioBridge`), automatically selects external display audio, and lets you hide/show the menu bar icon.

## Build & Test

```bash
make test
make all
```

## Install / Uninstall

```bash
./scripts/install.sh
open -a HDMIVolume
```

If you hide the menu bar icon from the menu (`Hide Menu Bar Icon`), re-open `HDMIVolume.app` (from Spotlight, Finder, or `open -a HDMIVolume`) to show it again.

To uninstall completely:

```bash
./scripts/uninstall.sh
```
