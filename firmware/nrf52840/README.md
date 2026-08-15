# Tinycardia nRF52840 firmware

This directory contains the Tinycardia v2 firmware for Nordic's nRF52840. It is
an nRF Connect SDK/Zephyr application and is intentionally isolated from the
legacy STM32 firmware in `../stm32`.

## Prerequisites

- nRF Connect SDK v3.4.0 installed and activated
- A Pro Micro nRF52840-compatible board using the UF2 bootloader
- `west` available in the activated SDK environment
- Repository submodules initialized with `git submodule update --init --recursive`

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

For first-board bring-up, connect to the USB CDC ACM serial port after flashing
(typically `/dev/ttyACM0`) at 115200 baud, then hold the power button for three
seconds. A working MAX30003 path reports the full `INFO` register and successful
configuration readback. For example:

```text
MAX30003 connected: INFO=0x5xxxxx, revision=x
MAX30003 register readback passed; ECG configured for 256 sps
```

An invalid `INFO` value points to the SPI/power path. A register readback
mismatch usually narrows the fault to CSB or MOSI, while a valid identity and
configuration with no sample lines points to INT1, the 32.768 kHz clock, or the
ECG FIFO path. In that case, a once-per-second health warning includes `STATUS`
and the logical INT1 level; if FIFO data is ready, it also performs one polling
read so SPI/FIFO bring-up can continue while INT1 is being diagnosed.

To force a clean configuration after changing boards or devicetree files:

```sh
west build -p always --sysbuild -d firmware/nrf52840/build \
  -b promicro_nrf52840/nrf52840/uf2 firmware/nrf52840
```

The starter application remains in nRF52840 System OFF until the active-low
power button is held continuously for three seconds. A short press wakes the
device, but it returns to System OFF when the button is released. After a valid
hold, the blue status LED turns on, the application initializes the MAX30003,
and BLE advertising starts. The status LED is forced off before every entry to
System OFF, so it directly indicates whether the application is ON.

During hardware bring-up, `CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG=y` in
`prj.conf` logs the active-low P0.06 state transitions and accumulated hold time
for both power-on and runtime power-off. The application and ECG acquisition
run normally. An accepted runtime power-off hold is logged, then the status LED
turns off and the MCU enters System OFF, disconnecting the USB console. Set the
option to `n` only to suppress the detailed state-transition logs.

## ECG processing pipeline

The MAX30003 runs at 256 samples per second. The application uses two
non-overlapping 2,560-sample slots: while one completed 10-second window is
prepared, acquisition immediately fills the other. Normal preprocessing no
longer creates a gap in the acquired stream. If processing ever takes longer
than the entire following 10-second window, the bounded pipeline reports the
resulting backpressure through `samples_dropped`.

Window preparation standardizes the ECG samples, detects R peaks using the same
preprocessing as the model-training notebook, and produces these seven
standardized RR features for model inference:

- mean RR interval
- SDNN
- RMSSD
- pNN50
- pNN20
- Poincare SD1
- Poincare SD2

The completed window is quantized and evaluated by the canonical
`model/afib_detector_int8.tflite` artifact in `prepared_window_handler()`.
Inference runs synchronously on the ECG processing thread, never in an ISR,
while the other slot remains dedicated to acquisition. The two TFLM inputs are
the standardized ECG `[1, 2560, 1]` and standardized RR features `[1, 7]`.

The model is embedded in flash at build time and executed from a static tensor
arena. Configuration rejects any artifact whose SHA-256 differs from the
canonical deployment hash. Initialization also rejects changes to the artifact
size, schema, 21-node operator graph, tensor names/order, shapes, INT8 types, or
quantization parameters. The resolver registers only EXPAND_DIMS, CONV_2D, RESHAPE,
MAX_POOL_2D, MEAN, FULLY_CONNECTED, CONCATENATION, and SOFTMAX. The nRF52840
build enables the Zephyr CMSIS-NN TFLM kernels.

AFIB is notebook label index 0 and NORMAL is index 1, based on the notebook's
`LabelEncoder` class ordering. The selected softmax probability is encoded as
BLE confidence in the range 0..10000. A window is not classified unless its RR
features are valid and lead/contact quality remained good for the complete
window. Inference and its counter continue when the phone is disconnected,
unsubscribed, or ECG streaming is disabled; BLE notification is opportunistic.

## BLE application protocol

The firmware exposes the standard Battery Service plus a fixed Tinycardia
service for live ECG, inference results, device status, and acknowledged device
controls. Monitoring and live streaming have separate lifetimes, and BLE work
is queued away from MAX30003 acquisition. See [`BLE_PROTOCOL.md`](BLE_PROTOCOL.md)
for UUIDs, exact byte layouts, MTU behavior, application APIs, and concurrency
details.

## Verification

See [`TESTING.md`](TESTING.md) for native ztest/Twister commands, automated
coverage, golden-data limits, CI behavior, and the physical bench checklist.

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
