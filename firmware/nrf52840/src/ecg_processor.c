// SPDX-License-Identifier: MIT

#include "ecg_processor.h"

#include "ecg_processing.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ecg_processor, CONFIG_LOG_DEFAULT_LEVEL);

#define ECG_WINDOW_SLOT_COUNT 2U

struct ecg_window_slot {
	struct ecg_sample_window samples;
	uint32_t start_timestamp_ms;
	uint32_t end_timestamp_ms;
	uint32_t monitoring_generation;
	bool queued;
	bool processing;
};

static struct ecg_window_slot window_slots[ECG_WINDOW_SLOT_COUNT];
static struct ecg_processing_workspace processing_workspace;
static struct ecg_processing_result processing_result;

static struct k_spinlock capture_lock;
static int8_t capture_slot = -1;
static bool processor_initialized;
static bool monitoring_enabled;
static uint32_t monitoring_generation;
static size_t discarded_sample_count;
static ecg_window_handler_t prepared_window_handler;
static void *prepared_window_handler_data;

K_MSGQ_DEFINE(window_ready_queue, sizeof(uint8_t), ECG_WINDOW_SLOT_COUNT, 1);

static void reset_slot(uint8_t slot_index)
{
	struct ecg_window_slot *slot = &window_slots[slot_index];

	ecg_sample_window_reset(&slot->samples);
	slot->start_timestamp_ms = 0U;
	slot->end_timestamp_ms = 0U;
	slot->monitoring_generation = 0U;
	slot->queued = false;
	slot->processing = false;
}

static int find_available_slot(void)
{
	for (uint8_t index = 0U; index < ECG_WINDOW_SLOT_COUNT; ++index) {
		if (!window_slots[index].queued && !window_slots[index].processing) {
			return index;
		}
	}

	return -1;
}

static int prepare_window(struct ecg_window_slot *slot,
			  struct ecg_prepared_window *window)
{
	uint32_t start_cycles;
	int err;

	start_cycles = k_cycle_get_32();
	err = ecg_prepare_model_inputs(&slot->samples, &processing_workspace,
				       &processing_result);
	window->preparation_time_us = (uint32_t)k_cyc_to_us_floor64(
		(uint32_t)(k_cycle_get_32() - start_cycles));
	if (err < 0) {
		return err;
	}

	window->ecg_samples = slot->samples.samples;
	window->rr_features = processing_result.rr.features_standardized;
	window->rr_features_unscaled = processing_result.rr.features_unscaled;
	window->sample_count = slot->samples.count;
	window->r_peak_count = processing_result.r_peak_count;
	window->rr_features_valid = processing_result.rr.features_valid;
	window->start_timestamp_ms = slot->start_timestamp_ms;
	window->end_timestamp_ms = slot->end_timestamp_ms;

	return 0;
}

static void ecg_processor_thread(void *arg1, void *arg2, void *arg3)
{
	struct ecg_prepared_window window;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		struct ecg_window_slot *slot;
		size_t discarded_samples;
		uint32_t slot_generation;
		uint8_t slot_index;
		bool publish_window;
		k_spinlock_key_t key;
		int err;

		k_msgq_get(&window_ready_queue, &slot_index, K_FOREVER);

		key = k_spin_lock(&capture_lock);
		slot = &window_slots[slot_index];
		if (!slot->queued) {
			k_spin_unlock(&capture_lock, key);
			continue;
		}
		slot->queued = false;
		slot->processing = true;
		slot_generation = slot->monitoring_generation;
		k_spin_unlock(&capture_lock, key);

		err = prepare_window(slot, &window);
		key = k_spin_lock(&capture_lock);
		publish_window = monitoring_enabled &&
				slot_generation == monitoring_generation;
		k_spin_unlock(&capture_lock, key);
		if (err < 0) {
			LOG_ERR("ECG window preparation failed: %d", err);
		} else if (publish_window && prepared_window_handler != NULL) {
			prepared_window_handler(&window, prepared_window_handler_data);
		}

		key = k_spin_lock(&capture_lock);
		reset_slot(slot_index);
		if (monitoring_enabled && capture_slot < 0) {
			capture_slot = slot_index;
		}
		discarded_samples = discarded_sample_count;
		discarded_sample_count = 0U;
		k_spin_unlock(&capture_lock, key);

		if (discarded_samples > 0U) {
			LOG_WRN("ECG capture backpressure dropped %u samples",
				(unsigned int)discarded_samples);
		}
	}
}

K_THREAD_DEFINE(ecg_processor_thread_id, CONFIG_TINYCARDIA_ECG_THREAD_STACK_SIZE,
		ecg_processor_thread, NULL, NULL, NULL, CONFIG_TINYCARDIA_ECG_THREAD_PRIORITY, 0,
		0);

int ecg_processor_init(ecg_window_handler_t window_handler, void *user_data)
{
	k_spinlock_key_t key;

	key = k_spin_lock(&capture_lock);
	if (processor_initialized) {
		k_spin_unlock(&capture_lock, key);
		return -EALREADY;
	}

	prepared_window_handler = window_handler;
	prepared_window_handler_data = user_data;
	for (uint8_t index = 0U; index < ECG_WINDOW_SLOT_COUNT; ++index) {
		reset_slot(index);
	}
	discarded_sample_count = 0U;
	monitoring_generation = 1U;
	monitoring_enabled = true;
	capture_slot = 0;
	processor_initialized = true;
	k_spin_unlock(&capture_lock, key);

	return 0;
}

int ecg_processor_set_monitoring(bool enabled)
{
	k_spinlock_key_t key;
	int available_slot;

	key = k_spin_lock(&capture_lock);
	if (!processor_initialized) {
		k_spin_unlock(&capture_lock, key);
		return -EACCES;
	}
	if (monitoring_enabled == enabled) {
		k_spin_unlock(&capture_lock, key);
		return 0;
	}

	if (!enabled) {
		monitoring_enabled = false;
		++monitoring_generation;
		capture_slot = -1;
		for (uint8_t index = 0U; index < ECG_WINDOW_SLOT_COUNT; ++index) {
			if (!window_slots[index].processing) {
				reset_slot(index);
			}
		}
		k_spin_unlock(&capture_lock, key);
		k_msgq_purge(&window_ready_queue);
		return 0;
	}

	available_slot = find_available_slot();
	if (available_slot < 0) {
		k_spin_unlock(&capture_lock, key);
		return -EBUSY;
	}
	++monitoring_generation;
	monitoring_enabled = true;
	reset_slot((uint8_t)available_slot);
	capture_slot = (int8_t)available_slot;
	discarded_sample_count = 0U;
	k_spin_unlock(&capture_lock, key);

	return 0;
}

bool ecg_processor_submit_sample(uint32_t raw_word, uint32_t timestamp_ms)
{
	struct ecg_window_slot *slot;
	enum ecg_window_append_result append_result;
	uint8_t completed_slot = 0U;
	bool window_ready = false;
	bool preserved = true;
	float sample_mv;
	k_spinlock_key_t key;

	sample_mv = ecg_decode_sample_mv(raw_word);
	key = k_spin_lock(&capture_lock);
	if (!monitoring_enabled || capture_slot < 0) {
		if (monitoring_enabled) {
			++discarded_sample_count;
		}
		k_spin_unlock(&capture_lock, key);
		return false;
	}

	completed_slot = (uint8_t)capture_slot;
	slot = &window_slots[completed_slot];
	if (slot->samples.count == 0U) {
		slot->start_timestamp_ms = timestamp_ms;
	}
	append_result = ecg_sample_window_append(&slot->samples, sample_mv);
	if (append_result == ECG_WINDOW_COMPLETED) {
		int next_slot;

		slot->end_timestamp_ms = timestamp_ms;
		slot->monitoring_generation = monitoring_generation;
		slot->queued = true;
		next_slot = find_available_slot();
		if (next_slot >= 0) {
			reset_slot((uint8_t)next_slot);
			capture_slot = (int8_t)next_slot;
		} else {
			capture_slot = -1;
		}
		window_ready = true;
	}
	k_spin_unlock(&capture_lock, key);

	if (window_ready &&
	    k_msgq_put(&window_ready_queue, &completed_slot, K_NO_WAIT) < 0) {
		key = k_spin_lock(&capture_lock);
		if (window_slots[completed_slot].queued) {
			reset_slot(completed_slot);
			if (monitoring_enabled && capture_slot < 0) {
				capture_slot = completed_slot;
			}
		}
		++discarded_sample_count;
		k_spin_unlock(&capture_lock, key);
		preserved = false;
		LOG_ERR("ECG processing queue full");
	}

	return preserved;
}

void ecg_processor_sample_handler(uint32_t raw_word, void *user_data)
{
	ARG_UNUSED(user_data);
	(void)ecg_processor_submit_sample(raw_word, k_uptime_get_32());
}
