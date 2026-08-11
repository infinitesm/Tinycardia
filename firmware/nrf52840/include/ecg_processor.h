/* SPDX-License-Identifier: MIT */

#ifndef TINYCARDIA_ECG_PROCESSOR_H_
#define TINYCARDIA_ECG_PROCESSOR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ECG_PROCESSOR_SAMPLE_RATE_HZ   256U
#define ECG_PROCESSOR_WINDOW_SECONDS   10U
#define ECG_PROCESSOR_WINDOW_SIZE      (ECG_PROCESSOR_SAMPLE_RATE_HZ * ECG_PROCESSOR_WINDOW_SECONDS)
#define ECG_PROCESSOR_RR_FEATURE_COUNT 7U

enum ecg_rr_feature_index {
	ECG_RR_MEAN_RR_MS,
	ECG_RR_SDNN_MS,
	ECG_RR_RMSSD_MS,
	ECG_RR_PNN50,
	ECG_RR_PNN20,
	ECG_RR_SD1_MS,
	ECG_RR_SD2_MS,
};

/**
 * Prepared model inputs for one non-overlapping ECG window.
 *
 * The ECG samples and RR features are standardized using the same constants as
 * the model-training pipeline. The unscaled RR features are provided for
 * diagnostics. All pointers remain valid only for the duration of the handler.
 */
struct ecg_prepared_window {
	const float *ecg_samples;
	const float *rr_features;
	const float *rr_features_unscaled;
	size_t sample_count;
	size_t r_peak_count;
	bool rr_features_valid;
};

typedef void (*ecg_window_handler_t)(const struct ecg_prepared_window *window, void *user_data);

/**
 * Initialize the ECG window processor.
 *
 * The window handler runs on the ECG processing thread. Capture remains paused
 * until the handler returns, which prevents overlap between consecutive
 * windows and leaves a safe insertion point for future inference.
 */
int ecg_processor_init(ecg_window_handler_t window_handler, void *user_data);

/** MAX30003 sample callback; pass this directly to max30003_init(). */
void ecg_processor_sample_handler(uint32_t raw_word, void *user_data);

#endif /* TINYCARDIA_ECG_PROCESSOR_H_ */
