# Firmware verification

This project uses a focused automated suite for signal and model-input integrity, plus a short
physical bench checklist for behavior that cannot be represented faithfully without the board.
The automated build is pinned to nRF Connect SDK v3.4.0 and its matching toolchain.

## Run locally

Open an nRF Connect SDK v3.4.0 terminal, then run these commands from the repository root:

```sh
west twister \
  -p native_sim/native/64 \
  -T firmware/nrf52840/tests \
  -O firmware/nrf52840/build/twister \
  --inline-logs

west build --sysbuild -p always \
  -b promicro_nrf52840/nrf52840/uf2 \
  -d firmware/nrf52840/build firmware/nrf52840
```

Twister builds and runs the ztest executable on the host. It does not require or emulate a
MAX30003, GPIO interrupts, BLE radio, or physical nRF52840. The firmware command builds the real
board image but does not flash it.

## Automated evidence

There were no existing requirement or verification IDs in the repository, so this table maps the
current risks directly to named test groups without inventing identifiers.

| Risk | Automated evidence | What is checked |
| --- | --- | --- |
| Corrupted MAX30003 samples | `ecg_decode` | Fixed zero, positive, negative, representative, and signed 18-bit boundary words; tag bits ignored; millivolt conversion |
| Shifted or stale model windows | `ecg_window` | Exact 2,560-sample ordering, completion boundary, partial window, full-buffer rejection, reset, and two non-overlapping consecutive windows |
| Incorrect timing features | `ecg_rr` | Fixed peak indices, millisecond RR intervals, feature order and values, normalization, insufficient peaks, extreme boundary intervals, and invalid peak lists |
| Silent preprocessing drift | `ecg_regression` | Deterministic 10-second ECG fixture with fixed R-peak indices, RR intervals, raw/standardized features, and all 2,560 standardized model-input samples |

Compiler warnings are errors in both the test application and production firmware. Twister test
reports and build logs are uploaded by CI, and the successful real-board build produces a UF2
artifact for convenience.

## Golden-data limits

The deterministic regression fixture is an analytic sequence of isolated triangular pulses. Its
expected outputs were fixed from the NumPy preprocessing definition in `model/Tinycardia.ipynb`,
not calculated by the C implementation under test. It is useful for detecting indexing, threshold,
normalization, and feature-order drift, but it is not a recorded or clinically representative ECG.

No redistributable recorded ECG window with reviewed expected peaks/model inputs is checked into
this repository. Add one when an approved, versioned fixture is available; until then, do not use
the analytic regression as evidence of clinical peak-detection performance. The nRF52840 suite
validates its own preprocessing contract and does not require STM32 implementation parity.

There is also no Tinycardia GATT service, notification characteristic, or binary payload codec in
the nRF52840 firmware yet. BLE currently advertises the standard Device Information Service, while
the desktop GUI parses UART text. Byte-order, signed-width, packet-length, and payload-boundary
tests cannot be added responsibly until that application protocol is defined.

## Physical bench checklist

Keep these checks on real hardware; they are intentionally excluded from CI:

- Read and confirm the MAX30003 ID/INFO value over SPI.
- Verify register configuration and readback against the intended 256 Hz setup.
- Verify FIFO interrupt delivery, sample ordering and rate, overflow handling, reset, and recovery.
- Inspect live ECG values for plausible amplitude, baseline, polarity, noise, and electrode-off
  behavior.
- Verify BLE advertising, connection, notification after a Tinycardia characteristic exists,
  disconnect, and reconnect behavior.
- Confirm a phone decodes the exact transmitted ECG, status, and classification bytes after the
  payload protocol exists.
- Verify the three-second power-button hold, System OFF entry, wake source, and restart behavior.
- Run end-to-end acquisition, preprocessing, inference, and classification with reviewed ECG
  inputs once inference is integrated.

Record the commit SHA, board revision, SDK/toolchain version, test operator, and captured logs for
each bench session. CI logs, Twister reports, and the commit SHA are the automated evidence record.
