// SPDX-License-Identifier: MIT

#include "ecg_processor.h"

#include <errno.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ecg_processor, CONFIG_LOG_DEFAULT_LEVEL);

#define MAX30003_ECG_SAMPLE_SHIFT 6U
#define MAX30003_ECG_SAMPLE_BITS  18U
#define MAX30003_ECG_SAMPLE_MASK  BIT_MASK(MAX30003_ECG_SAMPLE_BITS)
#define MAX30003_ECG_SIGN_BIT     BIT(MAX30003_ECG_SAMPLE_BITS - 1U)

/* Retained from the STM32 input conversion used to validate the model. */
#define MAX30003_ECG_LSB_MV (32.5f / 131072.0f)

#define QRS_INTEGRATION_WINDOW_SAMPLES ((15U * ECG_PROCESSOR_SAMPLE_RATE_HZ) / 100U)
#define QRS_INTEGRATION_LEFT_SAMPLES   (QRS_INTEGRATION_WINDOW_SAMPLES / 2U)
#define QRS_INTEGRATION_RIGHT_SAMPLES                                                              \
	(QRS_INTEGRATION_WINDOW_SAMPLES - QRS_INTEGRATION_LEFT_SAMPLES - 1U)

/* This interval must remain aligned with the preprocessing used to train the model. */
#define QRS_MIN_DISTANCE_SAMPLES   ((78U * ECG_PROCESSOR_SAMPLE_RATE_HZ) / 100U)
#define QRS_MAX_PEAKS              64U
#define ECG_MIN_STANDARD_DEVIATION 1.0e-6f

enum ecg_capture_state {
	ECG_CAPTURE_STOPPED,
	ECG_CAPTURE_FILLING,
	ECG_CAPTURE_PROCESSING,
};

static const float rr_feature_means[ECG_PROCESSOR_RR_FEATURE_COUNT] = {
	960.825116f, 99.9119008f, 146.620803f, 0.311937086f, 0.477063449f, 100.942917f, 74.6044621f,
};

static const float rr_feature_standard_deviations[ECG_PROCESSOR_RR_FEATURE_COUNT] = {
	149.60872127f, 137.881217f,   212.41114191f, 0.34092243f,
	0.32594487f,   123.60378762f, 119.86814799f,
};

static float ecg_samples[ECG_PROCESSOR_WINDOW_SIZE];
static float squared_derivative[ECG_PROCESSOR_WINDOW_SIZE];
static float integrated_signal[ECG_PROCESSOR_WINDOW_SIZE];
static size_t r_peak_indices[QRS_MAX_PEAKS];
static float rr_intervals_ms[QRS_MAX_PEAKS - 1U];
static float rr_features_unscaled[ECG_PROCESSOR_RR_FEATURE_COUNT];
static float rr_features_standardized[ECG_PROCESSOR_RR_FEATURE_COUNT];

static struct k_spinlock capture_lock;
static enum ecg_capture_state capture_state = ECG_CAPTURE_STOPPED;
static size_t captured_sample_count;
static size_t discarded_sample_count;
static ecg_window_handler_t prepared_window_handler;
static void *prepared_window_handler_data;

K_SEM_DEFINE(window_ready_sem, 0, 1);

BUILD_ASSERT(ECG_PROCESSOR_WINDOW_SIZE == 2560U, "The model requires exactly 2,560 ECG samples");
BUILD_ASSERT(QRS_INTEGRATION_WINDOW_SAMPLES > 0U, "The QRS integration window cannot be empty");

static float max30003_raw_to_mv(uint32_t raw_word)
{
	uint32_t unsigned_sample;
	int32_t signed_sample;

	unsigned_sample = (raw_word >> MAX30003_ECG_SAMPLE_SHIFT) & MAX30003_ECG_SAMPLE_MASK;
	if ((unsigned_sample & MAX30003_ECG_SIGN_BIT) != 0U) {
		signed_sample = (int32_t)(unsigned_sample | ~MAX30003_ECG_SAMPLE_MASK);
	} else {
		signed_sample = (int32_t)unsigned_sample;
	}

	return (float)signed_sample * MAX30003_ECG_LSB_MV;
}

static void standardize_ecg_window(void)
{
	float mean = 0.0f;
	float variance = 0.0f;
	float standard_deviation;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		mean += ecg_samples[index];
	}
	mean /= (float)ECG_PROCESSOR_WINDOW_SIZE;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		float difference = ecg_samples[index] - mean;

		variance += difference * difference;
	}
	standard_deviation = sqrtf(variance / (float)ECG_PROCESSOR_WINDOW_SIZE);
	if (standard_deviation < ECG_MIN_STANDARD_DEVIATION) {
		standard_deviation = ECG_MIN_STANDARD_DEVIATION;
	}

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		ecg_samples[index] = (ecg_samples[index] - mean) / standard_deviation;
	}
}

static void integrate_squared_derivative(void)
{
	float rolling_sum = 0.0f;

	squared_derivative[0] = 0.0f;
	for (size_t index = 1; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		float difference = ecg_samples[index] - ecg_samples[index - 1U];

		squared_derivative[index] = difference * difference;
	}

	for (size_t index = 0; index <= QRS_INTEGRATION_RIGHT_SAMPLES; ++index) {
		rolling_sum += squared_derivative[index];
	}

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		int32_t remove_index = (int32_t)index - (int32_t)QRS_INTEGRATION_LEFT_SAMPLES;
		size_t add_index = index + QRS_INTEGRATION_RIGHT_SAMPLES + 1U;

		integrated_signal[index] = rolling_sum / (float)QRS_INTEGRATION_WINDOW_SAMPLES;
		if (remove_index >= 0) {
			rolling_sum -= squared_derivative[remove_index];
		}
		if (add_index < ECG_PROCESSOR_WINDOW_SIZE) {
			rolling_sum += squared_derivative[add_index];
		}
	}
}

static size_t detect_r_peaks(void)
{
	float mean = 0.0f;
	float variance = 0.0f;
	float threshold;
	int32_t last_peak = -(int32_t)QRS_MIN_DISTANCE_SAMPLES - 1;
	size_t peak_count = 0U;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		mean += integrated_signal[index];
	}
	mean /= (float)ECG_PROCESSOR_WINDOW_SIZE;

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		float difference = integrated_signal[index] - mean;

		variance += difference * difference;
	}
	threshold = mean + 0.7f * sqrtf(variance / (float)ECG_PROCESSOR_WINDOW_SIZE);

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		if (integrated_signal[index] <= threshold) {
			continue;
		}
		if (((int32_t)index - last_peak) <= (int32_t)QRS_MIN_DISTANCE_SAMPLES) {
			continue;
		}

		r_peak_indices[peak_count++] = index;
		last_peak = (int32_t)index;
		if (peak_count == ARRAY_SIZE(r_peak_indices)) {
			break;
		}
	}

	return peak_count;
}

static bool extract_rr_features(size_t peak_count)
{
	size_t rr_count;
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

	for (size_t index = 0; index < ARRAY_SIZE(rr_features_unscaled); ++index) {
		rr_features_unscaled[index] = 0.0f;
	}

	if (peak_count < 3U) {
		return false;
	}

	rr_count = peak_count - 1U;
	for (size_t index = 0; index < rr_count; ++index) {
		rr_intervals_ms[index] =
			(float)(r_peak_indices[index + 1U] - r_peak_indices[index]) * 1000.0f /
			(float)ECG_PROCESSOR_SAMPLE_RATE_HZ;
		rr_sum += rr_intervals_ms[index];
	}
	mean_rr = rr_sum / (float)rr_count;

	for (size_t index = 0; index < rr_count; ++index) {
		float difference = rr_intervals_ms[index] - mean_rr;

		rr_squared_difference_sum += difference * difference;
	}
	rr_population_variance = rr_squared_difference_sum / (float)rr_count;

	difference_count = rr_count - 1U;
	for (size_t index = 0; index < difference_count; ++index) {
		float difference = rr_intervals_ms[index + 1U] - rr_intervals_ms[index];

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
		float difference = rr_intervals_ms[index + 1U] - rr_intervals_ms[index];
		float centered_difference = difference - mean_successive_difference;

		successive_variance_sum += centered_difference * centered_difference;
	}
	successive_population_variance = successive_variance_sum / (float)difference_count;

	rr_features_unscaled[ECG_RR_MEAN_RR_MS] = mean_rr;
	rr_features_unscaled[ECG_RR_SDNN_MS] =
		sqrtf(rr_squared_difference_sum / (float)(rr_count - 1U));
	rr_features_unscaled[ECG_RR_RMSSD_MS] =
		sqrtf(successive_squared_difference_sum / (float)difference_count);
	rr_features_unscaled[ECG_RR_PNN50] = (float)pnn50_count / (float)difference_count;
	rr_features_unscaled[ECG_RR_PNN20] = (float)pnn20_count / (float)difference_count;
	rr_features_unscaled[ECG_RR_SD1_MS] = sqrtf(successive_population_variance / 2.0f);

	sd2_squared = 2.0f * rr_population_variance - successive_population_variance / 2.0f;
	rr_features_unscaled[ECG_RR_SD2_MS] = sd2_squared > 0.0f ? sqrtf(sd2_squared) : 0.0f;

	return true;
}

static void standardize_rr_features(void)
{
	for (size_t index = 0; index < ARRAY_SIZE(rr_features_standardized); ++index) {
		rr_features_standardized[index] =
			(rr_features_unscaled[index] - rr_feature_means[index]) /
			rr_feature_standard_deviations[index];
	}
}

static void prepare_window(struct ecg_prepared_window *window)
{
	size_t peak_count;

	standardize_ecg_window();
	integrate_squared_derivative();
	peak_count = detect_r_peaks();

	window->rr_features_valid = extract_rr_features(peak_count);
	standardize_rr_features();

	window->ecg_samples = ecg_samples;
	window->rr_features = rr_features_standardized;
	window->rr_features_unscaled = rr_features_unscaled;
	window->sample_count = ECG_PROCESSOR_WINDOW_SIZE;
	window->r_peak_count = peak_count;
}

static void ecg_processor_thread(void *arg1, void *arg2, void *arg3)
{
	struct ecg_prepared_window window;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		size_t discarded_samples;
		k_spinlock_key_t key;

		k_sem_take(&window_ready_sem, K_FOREVER);
		prepare_window(&window);

		if (prepared_window_handler != NULL) {
			prepared_window_handler(&window, prepared_window_handler_data);
		}

		key = k_spin_lock(&capture_lock);
		discarded_samples = discarded_sample_count;
		discarded_sample_count = 0U;
		captured_sample_count = 0U;
		capture_state = ECG_CAPTURE_FILLING;
		k_spin_unlock(&capture_lock, key);

		LOG_INF("ECG capture restarted; discarded %u samples while processing",
			(unsigned int)discarded_samples);
	}
}

K_THREAD_DEFINE(ecg_processor_thread_id, CONFIG_TINYCARDIA_ECG_THREAD_STACK_SIZE,
		ecg_processor_thread, NULL, NULL, NULL, CONFIG_TINYCARDIA_ECG_THREAD_PRIORITY, 0,
		0);

int ecg_processor_init(ecg_window_handler_t window_handler, void *user_data)
{
	k_spinlock_key_t key;

	key = k_spin_lock(&capture_lock);
	if (capture_state != ECG_CAPTURE_STOPPED) {
		k_spin_unlock(&capture_lock, key);
		return -EALREADY;
	}

	prepared_window_handler = window_handler;
	prepared_window_handler_data = user_data;
	captured_sample_count = 0U;
	discarded_sample_count = 0U;
	capture_state = ECG_CAPTURE_FILLING;
	k_spin_unlock(&capture_lock, key);

	return 0;
}

void ecg_processor_sample_handler(uint32_t raw_word, void *user_data)
{
	bool window_ready = false;
	float sample_mv;
	k_spinlock_key_t key;

	ARG_UNUSED(user_data);

	sample_mv = max30003_raw_to_mv(raw_word);
	key = k_spin_lock(&capture_lock);
	if (capture_state == ECG_CAPTURE_PROCESSING) {
		++discarded_sample_count;
		k_spin_unlock(&capture_lock, key);
		return;
	}
	if (capture_state != ECG_CAPTURE_FILLING) {
		k_spin_unlock(&capture_lock, key);
		return;
	}

	ecg_samples[captured_sample_count++] = sample_mv;
	if (captured_sample_count == ECG_PROCESSOR_WINDOW_SIZE) {
		capture_state = ECG_CAPTURE_PROCESSING;
		window_ready = true;
	}
	k_spin_unlock(&capture_lock, key);

	if (window_ready) {
		LOG_INF("Captured a complete %u-sample ECG window",
			(unsigned int)ECG_PROCESSOR_WINDOW_SIZE);
		k_sem_give(&window_ready_sem);
	}
}
