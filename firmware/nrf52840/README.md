# Tinycardia nRF52840 firmware

This directory contains the Tinycardia v2 firmware for Nordic's nRF52840. It is
an nRF Connect SDK/Zephyr application and is intentionally isolated from the
legacy STM32 firmware in `../stm32`.

## Prerequisites

- nRF Connect SDK installed and activated
- A Pro Micro nRF52840-compatible board using the UF2 bootloader
- `west` available in the activated SDK environment

## VS Code setup

1. Open the repository root (`Tinycardia-v2`) in VS Code.
2. Install the recommended **nRF Connect for VS Code** extension.
3. In the nRF Connect Welcome view, select **Install SDK**. This installs both
   the nRF Connect SDK and its matching toolchain; choose a stable SDK release.
4. Under Applications, select `firmware/nrf52840`, choose **Add Build
   Configuration**, and use board target `promicro_nrf52840/nrf52840/uf2`.
5. Select **Generate and Build**, then run **Developer: Reload Window** if stale
   `cannot open source file "zephyr/..."` diagnostics remain.

The workspace points C/C++ IntelliSense at
`firmware/nrf52840/build/nrf52840/compile_commands.json`, the location produced
by the nRF Connect extension's sysbuild configuration. That file does not exist
until the first successful Zephyr configuration/build, so header diagnostics
before that build are expected.

## Build and flash

From the repository root:

```sh
west build --sysbuild -d firmware/nrf52840/build \
  -b promicro_nrf52840/nrf52840/uf2 firmware/nrf52840
west flash -d firmware/nrf52840/build
```

To force a clean configuration after changing boards or devicetree files:

```sh
west build -p always --sysbuild -d firmware/nrf52840/build \
  -b promicro_nrf52840/nrf52840/uf2 firmware/nrf52840
```

The starter application remains in nRF52840 System OFF until the active-low
power button is held continuously for three seconds. A short press wakes the
device, but it returns to System OFF when the button is released. After a valid
hold, the application initializes the MAX30003 and starts BLE advertising.

## ECG processing pipeline

The MAX30003 runs at 256 samples per second. The application collects exactly
2,560 samples into a 10-second window, pauses capture while that window is
prepared, then discards it and starts a new window from zero. Consecutive
windows never overlap.

Window preparation standardizes the ECG samples, detects R peaks using the same
preprocessing as the model-training notebook, and produces these seven
standardized RR features for the future inference input:

- mean RR interval
- SDNN
- RMSSD
- pNN50
- pNN20
- Poincare SD1
- Poincare SD2

The inference insertion point is `prepared_window_handler()` in `src/main.c`.
Capture remains paused until that handler returns, so inference can safely read
the prepared arrays without them being overwritten.

## Hardware mapping

The MAX30003 uses SPI mode 0 at up to 4 MHz. Both INT1 and the power button are
configured active-low with internal pull-ups.

| Signal | nRF52840 pin |
| --- | --- |
| MAX30003 MISO | P0.17 |
| MAX30003 MOSI | P0.20 |
| MAX30003 SCK | P0.22 |
| MAX30003 CSB | P0.24 |
| MAX30003 INT1 | P1.00 |
| Power button | P0.06 |

## Layout

- `src/` — application and driver source
- `include/` — application headers
- `boards/` — board-specific overlays and configuration fragments
- `prj.conf` — Zephyr Kconfig options
- `CMakeLists.txt` — Zephyr build definition
