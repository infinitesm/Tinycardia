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
	uint32_t start_timestamp_ms;
	uint32_t end_timestamp_ms;
	uint32_t preparation_time_us;
};

typedef void (*ecg_window_handler_t)(const struct ecg_prepared_window *window, void *user_data);

/**
 * Initialize the ECG window processor.
 *
 * The window handler runs on the ECG processing thread and owns its completed
 * slot until it returns. Acquisition concurrently fills the second slot, so
 * normal preprocessing does not create a sampling gap.
 */
int ecg_processor_init(ecg_window_handler_t window_handler, void *user_data);

/** Start or stop normal 10-second window capture and preprocessing. */
int ecg_processor_set_monitoring(bool enabled);

/**
 * Submit one acquired sample with its acquisition timestamp.
 *
 * Returns true when the analysis path preserved the sample and false when
 * monitoring was stopped or both bounded window slots were occupied.
 */
bool ecg_processor_submit_sample(uint32_t raw_word, uint32_t timestamp_ms);

/** Compatibility callback using current uptime as the sample timestamp. */
void ecg_processor_sample_handler(uint32_t raw_word, void *user_data);

#endif /* TINYCARDIA_ECG_PROCESSOR_H_ */
