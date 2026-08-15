# Tinycardia BLE protocol v1

Tinycardia is a BLE peripheral. Advertising contains only the general-discovery
flags and the Tinycardia service UUID; the complete device name is in the scan
response. ECG, inference, status, and other telemetry are never advertised.
Pairing and bonding policy are outside protocol v1.

All multibyte custom fields are serialized explicitly in little-endian order.
The firmware does not transmit native C structures.

## GATT services

The standard Battery Service exposes Battery Level (`0x2A19`) with READ and
NOTIFY. No battery ADC implementation exists yet. Until a board-specific
measurement calls `tinycardia_ble_battery_set_level()`, a Battery Level read
returns an ATT error rather than a fabricated percentage.

The fixed vendor UUID family is defined in `include/ble_service.h`:

| Purpose | UUID | Properties |
| --- | --- | --- |
| Tinycardia service | `f8a50001-7c5b-4e91-a6d2-3b1c9e4f5200` | Primary service |
| ECG Stream | `f8a50002-7c5b-4e91-a6d2-3b1c9e4f5200` | NOTIFY |
| Inference Result | `f8a50003-7c5b-4e91-a6d2-3b1c9e4f5200` | NOTIFY |
| Device Status | `f8a50004-7c5b-4e91-a6d2-3b1c9e4f5200` | READ, NOTIFY |
| Device Control | `f8a50005-7c5b-4e91-a6d2-3b1c9e4f5200` | WRITE with response |

## Wire formats

Every packet starts with version `0x01`.

### ECG Stream

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | version (`uint8_t`) |
| 1 | 4 | packet sequence (`uint32_t`) |
| 5 | 4 | first sample's acquisition uptime in ms (`uint32_t`) |
| 9 | 1 | sample count N (`uint8_t`) |
| 10 | 4 × N | unscaled signed MAX30003 samples (`int32_t[]`) |

N is normally 10, producing a 50-byte value and requiring ATT MTU 53 or
larger. At a smaller negotiated MTU, the firmware selects the largest whole
sample count that fits the same packet format. For example, the default ATT MTU
23 carries two samples per 18-byte value. One packet is never fragmented by the
application. The sequence starts at zero after boot and wraps naturally.

### Inference Result

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | version |
| 1 | 4 | inference ID |
| 5 | 4 | analysis-window end uptime in ms |
| 9 | 1 | classification: NORMAL=0, AFIB=1, UNKNOWN=2 |
| 10 | 1 | quality: GOOD=0, POOR=1, LEAD_OFF=2, UNKNOWN=3 |
| 11 | 2 | confidence: 0–10000, or unavailable=`0xFFFF` |

The value is exactly 13 bytes. The application calls
`tinycardia_ble_inference_publish()` only after a real inference completes; the
BLE layer does not generate placeholder classifications.

### Device Status

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | version |
| 1 | 4 | uptime in seconds |
| 5 | 4 | samples acquired |
| 9 | 4 | samples dropped |
| 13 | 4 | completed inference count |
| 17 | 1 | lead status |
| 18 | 1 | operating state |

The value is exactly 19 bytes. Lead status values are GOOD=0, LEAD_1_OFF=1,
LEAD_2_OFF=2, BOTH_OFF=3, CHECKING=4, and UNKNOWN=5. The current MAX30003
mapping treats its negative ECG electrode as lead/contact 1 and its positive ECG
electrode as lead/contact 2. The STATUS register is polled at the configurable
`CONFIG_TINYCARDIA_MAX30003_STATUS_POLL_MS` interval (default 1000 ms), while
BLE status notifications are emitted only for meaningful transitions.

Operating states are IDLE=0, MONITORING=1,
MONITORING_AND_STREAMING=2, and ERROR=3.

### Device Control

The acknowledged write value must contain exactly one byte:

| Value | Command |
| --- | --- |
| `0x01` | START_STREAM |
| `0x02` | STOP_STREAM |
| `0x03` | START_MONITORING |
| `0x04` | STOP_MONITORING |

Monitoring owns MAX30003 acquisition, preprocessing, lead/contact checks, and
future inference. Streaming owns only live ECG transport. START_STREAM requires
monitoring, a connection, an ECG subscription, and room for at least one
sample. STOP_STREAM never stops monitoring. STOP_MONITORING also clears
streaming to keep the state consistent. A disconnect clears connection
streaming and subscriptions but monitoring continues; advertising restarts
after the connection is recycled.

## Data flow and execution contexts

```text
MAX30003 INT1 ISR
  -> submit FIFO work only
  -> system work queue performs SPI/FIFO reads
     -> ECG processor's bounded 2,560-sample analysis window
     -> bounded BLE ECG message queue
        -> dedicated BLE work queue packetizes and notifies

completed inference callback
  -> tinycardia_ble_inference_publish()
  -> bounded inference message queue
  -> dedicated BLE work queue notifies
```

Acquisition never waits for BLE. The GPIO ISR performs no SPI or BLE work.
Shared connection/protocol state is mutex-protected, counters are atomic, and
the queues use nonblocking producer operations. Two analysis-window slots let
preprocessing operate on one complete window while acquisition fills the next.
Known analysis, MAX30003 FIFO, or BLE backpressure losses increment
`samples_dropped`; samples ignored while streaming is intentionally disabled do
not. Because the MAX30003 does not expose an exact overflow loss count, each
overflow contributes a conservative lower bound and re-anchors subsequent
uptime timestamps.

The MAX30003 timestamps samples from the acquisition epoch at the deterministic
256 Hz rate. ECG packets retain the first queued sample's timestamp rather than
using notification time. Prepared analysis windows retain their last sample's
timestamp for the future inference call. Monitoring restarts establish a new
session boundary, so late results from an earlier session are rejected even
across the 32-bit uptime wrap.

Queued inference notifications carry a connection generation and are purged on
unsubscribe or disconnect, preventing an old connection's result from reaching
a newly connected phone. Re-advertising uses a one-second retry after transient
failures.

## Application integration points

- `tinycardia_ble_ecg_sample()` is connected to real MAX30003 samples.
- `tinycardia_ble_inference_publish()` is ready at the prepared-window callback;
  a classifier is not yet integrated.
- `tinycardia_ble_battery_set_level()` is ready for a future battery driver; no
  percentage is invented.
- MAX30003 lead-off transitions call `tinycardia_ble_status_set_lead()`.
- `tinycardia_ble_status_set_error()` provides an explicit ERROR transition.
