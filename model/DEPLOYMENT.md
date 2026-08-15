# Tinycardia MCU deployment artifact

Canonical artifact: `afib_detector_int8.tflite`

- Size: 91,816 bytes
- SHA-256: `85a9a57433d27c7a059edb1f5a4752c965d2f0d745e23dddd530fddf320c50b6`
- TFLite subgraphs: 1
- Operator instances: 21

## Tensor contract

| Role | Tensor name | Shape | Type | Scale | Zero point |
| --- | --- | --- | --- | --- | --- |
| RR input 0 | `serving_default_rr_features:0` | `[1, 7]` | INT8 | `0.014579945243895054` | `-27` |
| ECG input 1 | `serving_default_ecg_signal:0` | `[1, 2560, 1]` | INT8 | `0.04720002040266991` | `-15` |
| Output 0 | `StatefulPartitionedCall_1:0` | `[1, 2]` | INT8 | `0.00390625` | `-128` |

The graph requires EXPAND_DIMS, CONV_2D, RESHAPE, MAX_POOL_2D, MEAN,
FULLY_CONNECTED, CONCATENATION, and SOFTMAX. CMake validates the canonical
SHA-256 before embedding the artifact. Firmware initialization validates its
size, schema, graph operator set/count, and full tensor contract before
monitoring starts.

The notebook fits `LabelEncoder` on the two text labels in alphabetical order,
so output index 0 is `atrial fibrillation` and output index 1 is `sinus normal`.

An independent LiteRT 2.2.0 invocation of the canonical artifact with both
real-valued inputs set to zero produces raw INT8 output `[-49, 49]`. Firmware
initialization executes this sentinel vector and accepts at most one INT8 LSB
of kernel variation. This exercises tensor allocation and the selected target
kernels before acquisition starts. The native test expects the exact reference
result, NORMAL classification, and confidence 6914.

## Reference-vector status

The available verified reference contains all seven RR INT8 values and only
the first ten of 2,560 ECG INT8 values. The missing 2,550 values are not
reconstructed or replaced. Consequently, automated tests cover exact
round-trip quantization of the available values, output dequantization,
metadata, arena allocation, and an independently checked deterministic model
invocation, but not the historical
golden output `[0.0, 0.99609375]`. Add that comparison only when the complete
reviewed input vector is available.
