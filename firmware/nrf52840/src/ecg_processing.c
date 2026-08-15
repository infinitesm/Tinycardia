// SPDX-License-Identifier: MIT

#include "ecg_processing.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define MAX30003_ECG_SAMPLE_SHIFT 6U
#define MAX30003_ECG_SAMPLE_MASK  0x3ffffU
#define MAX30003_ECG_SIGN_BIT     0x20000U

/* Retained from the STM32 input conversion used to validate the model. */
#define MAX30003_ECG_LSB_MV (32.5f / 131072.0f)

#define QRS_INTEGRATION_WINDOW_SAMPLES ((15U * ECG_PROCESSOR_SAMPLE_RATE_HZ) / 100U)
#define QRS_INTEGRATION_LEFT_SAMPLES   (QRS_INTEGRATION_WINDOW_SAMPLES / 2U)
#define QRS_INTEGRATION_RIGHT_SAMPLES                                                      \
	(QRS_INTEGRATION_WINDOW_SAMPLES - QRS_INTEGRATION_LEFT_SAMPLES - 1U)

/* This interval must remain aligned with the preprocessing used to train the model. */
#define QRS_MIN_DISTANCE_SAMPLES   ((78U * ECG_PROCESSOR_SAMPLE_RATE_HZ) / 100U)
#define ECG_MIN_STANDARD_DEVIATION 1.0e-6f

static const float rr_feature_means[ECG_PROCESSOR_RR_FEATURE_COUNT] = {
	960.825116f, 99.9119008f, 146.620803f, 0.311937086f, 0.477063449f, 100.942917f,
	74.6044621f,
};

static const float rr_feature_standard_deviations[ECG_PROCESSOR_RR_FEATURE_COUNT] = {
	149.60872127f, 137.881217f,   212.41114191f, 0.34092243f,
	0.32594487f,   123.60378762f, 119.86814799f,
};

_Static_assert(ECG_PROCESSOR_WINDOW_SIZE == 2560U,
	       "The model requires exactly 2,560 ECG samples");
_Static_assert(QRS_INTEGRATION_WINDOW_SAMPLES > 0U,
	       "The QRS integration window cannot be empty");

int32_t ecg_decode_raw_sample(uint32_t raw_word)
{
	uint32_t unsigned_sample;

	unsigned_sample = (raw_word >> MAX30003_ECG_SAMPLE_SHIFT) & MAX30003_ECG_SAMPLE_MASK;
	if ((unsigned_sample & MAX30003_ECG_SIGN_BIT) != 0U) {
		return (int32_t)(unsigned_sample | ~MAX30003_ECG_SAMPLE_MASK);
	}

	return (int32_t)unsigned_sample;
}

float ecg_decode_sample_mv(uint32_t raw_word)
{
	return (float)ecg_decode_raw_sample(raw_word) * MAX30003_ECG_LSB_MV;
}

void ecg_sample_window_reset(struct ecg_sample_window *window)
{
	if (window != NULL) {
		window->count = 0U;
	}
}

enum ecg_window_append_result ecg_sample_window_append(struct ecg_sample_window *window,
							       float sample)
{
	if (window == NULL || window->count >= ECG_PROCESSOR_WINDOW_SIZE) {
		return ECG_WINDOW_ALREADY_FULL;
	}

	window->samples[window->count++] = sample;
	if (window->count == ECG_PROCESSOR_WINDOW_SIZE) {
		return ECG_WINDOW_COMPLETED;
	}

	return ECG_WINDOW_SAMPLE_STORED;
}

static void standardize_ecg_window(float samples[ECG_PROCESSOR_WINDOW_SIZE])
{
	float mean = 0.0f;
	float variance = 0.0f;
	float standard_deviation;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		mean += samples[index];
	}
	mean /= (float)ECG_PROCESSOR_WINDOW_SIZE;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		float difference = samples[index] - mean;

		variance += difference * difference;
	}
	standard_deviation = sqrtf(variance / (float)ECG_PROCESSOR_WINDOW_SIZE);
	if (standard_deviation < ECG_MIN_STANDARD_DEVIATION) {
		standard_deviation = ECG_MIN_STANDARD_DEVIATION;
	}

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		samples[index] = (samples[index] - mean) / standard_deviation;
	}
}

static void integrate_squared_derivative(
	const float samples[ECG_PROCESSOR_WINDOW_SIZE],
	struct ecg_processing_workspace *workspace)
{
	float rolling_sum = 0.0f;

	workspace->squared_derivative[0] = 0.0f;
	for (size_t index = 1; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		float difference = samples[index] - samples[index - 1U];

		workspace->squared_derivative[index] = difference * difference;
	}

	for (size_t index = 0; index <= QRS_INTEGRATION_RIGHT_SAMPLES; ++index) {
		rolling_sum += workspace->squared_derivative[index];
	}

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		int32_t remove_index = (int32_t)index - (int32_t)QRS_INTEGRATION_LEFT_SAMPLES;
		size_t add_index = index + QRS_INTEGRATION_RIGHT_SAMPLES + 1U;

		workspace->integrated_signal[index] =
			rolling_sum / (float)QRS_INTEGRATION_WINDOW_SAMPLES;
		if (remove_index >= 0) {
			rolling_sum -= workspace->squared_derivative[remove_index];
		}
		if (add_index < ECG_PROCESSOR_WINDOW_SIZE) {
			rolling_sum += workspace->squared_derivative[add_index];
		}
	}
}

static size_t detect_r_peaks(struct ecg_processing_workspace *workspace)
{
	float mean = 0.0f;
	float variance = 0.0f;
	float threshold;
	int32_t last_peak = -(int32_t)QRS_MIN_DISTANCE_SAMPLES - 1;
	size_t peak_count = 0U;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		mean += workspace->integrated_signal[index];
	}
	mean /= (float)ECG_PROCESSOR_WINDOW_SIZE;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		float difference = workspace->integrated_signal[index] - mean;

		variance += difference * difference;
	}
	threshold = mean + 0.7f * sqrtf(variance / (float)ECG_PROCESSOR_WINDOW_SIZE);

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		if (workspace->integrated_signal[index] <= threshold) {
			continue;
		}
		if (((int32_t)index - last_peak) <= (int32_t)QRS_MIN_DISTANCE_SAMPLES) {
			continue;
		}

		workspace->r_peak_indices[peak_count++] = index;
		last_peak = (int32_t)index;
		if (peak_count == ECG_PROCESSING_MAX_R_PEAKS) {
			break;
		}
	}

	return peak_count;
}

static void standardize_rr_features(struct ecg_rr_result *result)
{
	for (size_t index = 0; index < ECG_PROCESSOR_RR_FEATURE_COUNT; ++index) {
		result->features_standardized[index] =
			(result->features_unscaled[index] - rr_feature_means[index]) /
			rr_feature_standard_deviations[index];
	}
}

int ecg_extract_rr_features(const size_t *peak_indices, size_t peak_count,
			    struct ecg_rr_result *result)
{
	size_t difference_count;
	float rr_sum = 0.0f;
	float mean_rr;
	float rr_squared_difference_sum = 0.0f;
	float rr_population_variance;
	float successive_difference_sum = 0.0f;
	float successive_squared_difference_sum = 0.0f;
	float mean_successive_difference;
	float successive_variance_sum = 0.0f;
	float successive_population_variance;
	size_t pnn50_count = 0U;
	size_t pnn20_count = 0U;
	float sd2_squared;

	if (result == NULL || (peak_count > 0U && peak_indices == NULL)) {
		return -EINVAL;
	}
	if (peak_count > ECG_PROCESSING_MAX_R_PEAKS) {
		return -E2BIG;
	}

	memset(result, 0, sizeof(*result));
	for (size_t index = 0; index < peak_count; ++index) {
		if (peak_indices[index] >= ECG_PROCESSOR_WINDOW_SIZE) {
			return -ERANGE;
		}
		if (index > 0U && peak_indices[index] <= peak_indices[index - 1U]) {
			return -EINVAL;
		}
	}

	if (peak_count > 1U) {
		result->interval_count = peak_count - 1U;
	}
	for (size_t index = 0; index < result->interval_count; ++index) {
		result->intervals_ms[index] =
			(float)(peak_indices[index + 1U] - peak_indices[index]) * 1000.0f /
			(float)ECG_PROCESSOR_SAMPLE_RATE_HZ;
		rr_sum += result->intervals_ms[index];
	}

	if (result->interval_count < 2U) {
		standardize_rr_features(result);
		return 0;
	}

	mean_rr = rr_sum / (float)result->interval_count;
	for (size_t index = 0; index < result->interval_count; ++index) {
		float difference = result->intervals_ms[index] - mean_rr;

		rr_squared_difference_sum += difference * difference;
	}
	rr_population_variance =
		rr_squared_difference_sum / (float)result->interval_count;

	difference_count = result->interval_count - 1U;
	for (size_t index = 0; index < difference_count; ++index) {
		float difference = result->intervals_ms[index + 1U] - result->intervals_ms[index];

		successive_difference_sum += difference;
		successive_squared_difference_sum += difference * difference;
		if (fabsf(difference) > 50.0f) {
			++pnn50_count;
		}
		if (fabsf(difference) > 20.0f) {
			++pnn20_count;
		}
	}
	mean_successive_difference = successive_difference_sum / (float)difference_count;

	for (size_t index = 0; index < difference_count; ++index) {
		float difference = result->intervals_ms[index + 1U] - result->intervals_ms[index];
		float centered_difference = difference - mean_successive_difference;

		successive_variance_sum += centered_difference * centered_difference;
	}
	successive_population_variance = successive_variance_sum / (float)difference_count;

	result->features_unscaled[ECG_RR_MEAN_RR_MS] = mean_rr;
	result->features_unscaled[ECG_RR_SDNN_MS] =
		sqrtf(rr_squared_difference_sum / (float)(result->interval_count - 1U));
	result->features_unscaled[ECG_RR_RMSSD_MS] =
		sqrtf(successive_squared_difference_sum / (float)difference_count);
	result->features_unscaled[ECG_RR_PNN50] = (float)pnn50_count / (float)difference_count;
	result->features_unscaled[ECG_RR_PNN20] = (float)pnn20_count / (float)difference_count;
	result->features_unscaled[ECG_RR_SD1_MS] =
		sqrtf(successive_population_variance / 2.0f);

	sd2_squared = 2.0f * rr_population_variance - successive_population_variance / 2.0f;
	result->features_unscaled[ECG_RR_SD2_MS] =
		sd2_squared > 0.0f ? sqrtf(sd2_squared) : 0.0f;
	result->features_valid = true;
	standardize_rr_features(result);

	return 0;
}

int ecg_prepare_model_inputs(struct ecg_sample_window *window,
			     struct ecg_processing_workspace *workspace,
			     struct ecg_processing_result *result)
{
	int err;

	if (window == NULL || workspace == NULL || result == NULL) {
		return -EINVAL;
	}
	if (window->count != ECG_PROCESSOR_WINDOW_SIZE) {
		return -ENODATA;
	}

	standardize_ecg_window(window->samples);
	integrate_squared_derivative(window->samples, workspace);
	result->r_peak_count = detect_r_peaks(workspace);
	err = ecg_extract_rr_features(workspace->r_peak_indices, result->r_peak_count,
				      &result->rr);
	if (err < 0) {
		return err;
	}

	return 0;
}
