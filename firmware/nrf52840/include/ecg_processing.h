/* SPDX-License-Identifier: MIT */

#ifndef TINYCARDIA_ECG_PROCESSING_H_
#define TINYCARDIA_ECG_PROCESSING_H_

#include "ecg_processor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ECG_PROCESSING_MAX_R_PEAKS 64U

enum ecg_window_append_result {
	ECG_WINDOW_SAMPLE_STORED,
	ECG_WINDOW_COMPLETED,
	ECG_WINDOW_ALREADY_FULL,
};

struct ecg_sample_window {
	float samples[ECG_PROCESSOR_WINDOW_SIZE];
	size_t count;
};

struct ecg_processing_workspace {
	float squared_derivative[ECG_PROCESSOR_WINDOW_SIZE];
	float integrated_signal[ECG_PROCESSOR_WINDOW_SIZE];
	size_t r_peak_indices[ECG_PROCESSING_MAX_R_PEAKS];
};

struct ecg_rr_result {
	float intervals_ms[ECG_PROCESSING_MAX_R_PEAKS - 1U];
	size_t interval_count;
	float features_unscaled[ECG_PROCESSOR_RR_FEATURE_COUNT];
	float features_standardized[ECG_PROCESSOR_RR_FEATURE_COUNT];
	bool features_valid;
};

struct ecg_processing_result {
	struct ecg_rr_result rr;
	size_t r_peak_count;
};

/** Extract the signed 18-bit ECG sample while ignoring FIFO tag/status bits. */
int32_t ecg_decode_raw_sample(uint32_t raw_word);

/** Decode one MAX30003 FIFO word and convert the sample to millivolts. */
float ecg_decode_sample_mv(uint32_t raw_word);

void ecg_sample_window_reset(struct ecg_sample_window *window);

enum ecg_window_append_result ecg_sample_window_append(struct ecg_sample_window *window,
							       float sample);

/**
 * Compute RR intervals and model features from ordered, in-window peak indices.
 *
 * Fewer than three peaks is a valid but unavailable result: intervals are
 * reported when possible, features are zero before standardization, and
 * features_valid is false.
 */
int ecg_extract_rr_features(const size_t *peak_indices, size_t peak_count,
			    struct ecg_rr_result *result);

/** Standardize a complete window and prepare both model input branches. */
int ecg_prepare_model_inputs(struct ecg_sample_window *window,
			     struct ecg_processing_workspace *workspace,
			     struct ecg_processing_result *result);

#endif /* TINYCARDIA_ECG_PROCESSING_H_ */
