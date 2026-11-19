# Curaleaf Garden firmware

Firmware for three garden-sim devices:

| Sketch | Board |
|---|---|
| `firmware/Curaleaf_GardenV5_S3.ino` | 4" ESP32-S3 CYD (ST7796, 480x320) |
| `firmware/Curaleaf_GardenV5_S28.ino` | 2.8" ESP32-S3 CYD (ILI9341, 320x240, capacitive touch) |
| `firmware/Curaleaf_GardenV5.ino` | Classic ESP32 CYD (ST7789, resistive touch) |

## Builds

Push to `main` compiles all three sketches with pinned toolchain
(arduino-cli, ESP32 core 3.3.12, pinned libraries). Artifacts are attached to
each Actions run.

## Releasing an update

1. Commit your firmware changes to `main`.
2. Tag it: `git tag v15 && git push origin v15`
3. GitHub Actions builds all three boards and publishes a Release containing
   `Curaleaf_GardenV5_S3.ino.bin`, `Curaleaf_GardenV5_S28.ino.bin`,
   `Curaleaf_GardenV5.ino.bin`, and `version.txt`.

Devices with the OTA feature installed can then update themselves over WiFi:
they read `version.txt`, compare it with their own build tag, download the
matching `.bin` for their board, and flash it without a computer.
