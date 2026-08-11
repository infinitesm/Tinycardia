// SPDX-License-Identifier: MIT

#include "ecg_processor.h"

#include "ecg_processing.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ecg_processor, CONFIG_LOG_DEFAULT_LEVEL);

enum ecg_capture_state {
	ECG_CAPTURE_STOPPED,
	ECG_CAPTURE_FILLING,
	ECG_CAPTURE_PROCESSING,
};

static struct ecg_sample_window sample_window;
static struct ecg_processing_workspace processing_workspace;
static struct ecg_processing_result processing_result;

static struct k_spinlock capture_lock;
static enum ecg_capture_state capture_state = ECG_CAPTURE_STOPPED;
static size_t discarded_sample_count;
static ecg_window_handler_t prepared_window_handler;
static void *prepared_window_handler_data;

K_SEM_DEFINE(window_ready_sem, 0, 1);

static int prepare_window(struct ecg_prepared_window *window)
{
	int err;

	err = ecg_prepare_model_inputs(&sample_window, &processing_workspace,
				       &processing_result);
	if (err < 0) {
		return err;
	}

	window->ecg_samples = sample_window.samples;
	window->rr_features = processing_result.rr.features_standardized;
	window->rr_features_unscaled = processing_result.rr.features_unscaled;
	window->sample_count = sample_window.count;
	window->r_peak_count = processing_result.r_peak_count;
	window->rr_features_valid = processing_result.rr.features_valid;

	return 0;
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
		int err;

		k_sem_take(&window_ready_sem, K_FOREVER);
		err = prepare_window(&window);
		if (err < 0) {
			LOG_ERR("ECG window preparation failed: %d", err);
		} else if (prepared_window_handler != NULL) {
			prepared_window_handler(&window, prepared_window_handler_data);
		}

		key = k_spin_lock(&capture_lock);
		discarded_samples = discarded_sample_count;
		discarded_sample_count = 0U;
		ecg_sample_window_reset(&sample_window);
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
	ecg_sample_window_reset(&sample_window);
	discarded_sample_count = 0U;
	capture_state = ECG_CAPTURE_FILLING;
	k_spin_unlock(&capture_lock, key);

	return 0;
}

void ecg_processor_sample_handler(uint32_t raw_word, void *user_data)
{
	bool window_ready = false;
	enum ecg_window_append_result append_result;
	float sample_mv;
	k_spinlock_key_t key;

	ARG_UNUSED(user_data);

	sample_mv = ecg_decode_sample_mv(raw_word);
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

	append_result = ecg_sample_window_append(&sample_window, sample_mv);
	if (append_result == ECG_WINDOW_COMPLETED) {
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
