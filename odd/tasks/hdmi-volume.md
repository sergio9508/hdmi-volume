# HDMI Volume Control (macOS)

Minimalist CoreAudio HAL loopback driver + menu bar companion app for controlling HDMI/DisplayPort audio volume via macOS keyboard volume keys.

## Specs

- S1: "controlar el audio con las teclas de volumen del teclado" — Expose a virtual CoreAudio output device (`HDMI Volume`) with writable native macOS volume and mute controls so keyboard volume/mute keys work natively and scale audio forwarded to the target HDMI/DisplayPort device.
- S2: "podes ocultar o mostrar el icono de la app en la barra superior de la mac" — Provide a menu bar status item (`NSStatusItem`) with an action to hide the icon, and restore the icon when the app is re-activated/re-launched (or toggled).

## Tasks

- [x] T1 | S1 | inline | pending | HAL `AudioServerPlugIn` (`HDMIVolume.driver`) with loopback streams, native volume/mute controls, and C unit tests
- [x] T2 | S1, S2 | inline | pending | Swift companion app (`HDMIVolume.app`) with CoreAudio AUHAL loopback-to-HDMI bridge, menu bar icon show/hide toggle, and Swift unit tests
- [x] T3 | S1, S2 | inline | pending | `Makefile`, `scripts/install.sh`, `scripts/uninstall.sh`, `README.md`, full build & test verification

## Log

- L1: "si dale en esa carpeta arranquemos, la idea es que pueda controlar el audio con las teclas de volumen del teclado y podes ocultar o mostrar el icono de la app en la barra superior de la mac"
