#ifndef TINYCARDIA_MODEL_INFERENCE_H_
#define TINYCARDIA_MODEL_INFERENCE_H_

#include "ble_protocol.h"
#include "model_contract.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tinycardia_model_result {
	enum tinycardia_classification classification;
	uint16_t confidence;
	int8_t quantized_probabilities[TINYCARDIA_MODEL_CLASS_COUNT];
	uint32_t invoke_time_us;
};

/** Initialize TFLM and validate the complete embedded model contract. */
int tinycardia_model_init(void);

/**
 * Quantize one prepared ECG/RR window and run inference synchronously.
 *
 * This function is called only from the ECG processing thread. It owns no
 * input buffers and does not publish BLE data.
 */
int tinycardia_model_infer(const float *ecg, size_t ecg_count, const float *rr_features,
			   size_t rr_count, struct tinycardia_model_result *result);

/** Actual bytes used within the statically allocated tensor arena. */
size_t tinycardia_model_arena_used_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYCARDIA_MODEL_INFERENCE_H_ */
