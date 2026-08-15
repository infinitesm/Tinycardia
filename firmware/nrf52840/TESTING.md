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
| Wire-format or byte-order drift | `ble_ecg_packet`, `ble_inference_packet`, `ble_status_packet` | Exact packet sizes and offsets, little-endian counters/timestamps, positive and negative signed samples, short packets, enum fields, confidence boundaries, and no structure-layout dependency |
| Invalid MTU packet sizing | `ble_ecg_packet` | Largest unfragmented sample count at boundary ATT MTUs, including the 53-byte full-packet threshold |
| Invalid controls or inconsistent state | `ble_control`, `ble_state` | Exact one-byte command validation, monitoring/streaming transitions, transport preconditions, STOP_STREAM independence, disconnect behavior, and STOP_MONITORING consistency |
| Analysis stalls acquisition | `ecg_processor` | A complete second 2,560-sample window is retained while the first window's handler is deliberately blocked |
| Stale work crosses monitoring sessions | `ecg_processor`, `ble_ecg_packet` | Queued windows are invalidated by STOP_MONITORING, restarted capture remains usable, and wrapping uptime timestamps reject earlier-session results |

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

The protocol codec and state-machine tests are host-native and independent of
the Bluetooth controller. They prove wire bytes and logical transitions, but
they do not prove over-the-air service discovery, CCC behavior, negotiated MTU,
or radio throughput. Those remain physical bench checks.

## Physical bench checklist

Keep these checks on real hardware; they are intentionally excluded from CI:

- Read and confirm the MAX30003 ID/INFO value over SPI.
- Verify register configuration and readback against the intended 256 Hz setup.
- Verify FIFO interrupt delivery, sample ordering and rate, overflow handling, reset, and recovery.
- Inspect live ECG values for plausible amplitude, baseline, polarity, noise, and electrode-off
  behavior.
- Verify the standard Battery Service and all four Tinycardia characteristics
  are discoverable with the documented UUIDs and properties.
- Negotiate ATT MTU 53 or larger and confirm 50-byte/10-sample ECG values; also
  repeat at ATT MTU 23 and confirm valid shorter packets.
- Subscribe, issue acknowledged start/stop controls, and confirm monitoring is
  independent of streaming and connection lifetime.
- Disconnect during streaming, confirm acquisition continues, reconnect, and
  confirm advertising/subscription/control recovery.
- Confirm a phone decodes the exact transmitted signed ECG and status bytes;
  confirm inference bytes after the real classifier is integrated.
- Exercise electrode disconnect/reconnect and confirm restrained Device Status
  transition notifications with the intended physical lead labels.
- Inject or provoke BLE backpressure where practical and confirm acquisition
  remains alive and known loss is reflected in `samples_dropped`.
- Verify the three-second power-button hold, System OFF entry, wake source, and restart behavior.
- Run end-to-end acquisition, preprocessing, inference, and classification with reviewed ECG
  inputs once inference is integrated.

Record the commit SHA, board revision, SDK/toolchain version, test operator, and captured logs for
each bench session. CI logs, Twister reports, and the commit SHA are the automated evidence record.
