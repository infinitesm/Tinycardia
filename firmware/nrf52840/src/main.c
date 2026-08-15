/* SPDX-License-Identifier: MIT */

#include "ble_service.h"
#include "ecg_processing.h"
#include "ecg_processor.h"
#include "max30003.h"
#include "power_control.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(tinycardia, CONFIG_LOG_DEFAULT_LEVEL);

static void prepared_window_handler(const struct ecg_prepared_window *window,
				    void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("ECG window prepared: %u samples, %u R peaks, RR features %s",
		(unsigned int)window->sample_count,
		(unsigned int)window->r_peak_count,
		window->rr_features_valid ? "ready" : "insufficient R peaks");

	/*
	 * Future inference consumes the prepared inputs here, then calls
	 * tinycardia_ble_inference_publish() with window->end_timestamp_ms.
	 * No placeholder classification is emitted.
	 */
}

static void live_ecg_sample_handler(uint32_t raw_word, uint32_t timestamp_ms,
				    void *user_data)
{
	bool processing_preserved;

	ARG_UNUSED(user_data);
	processing_preserved = ecg_processor_submit_sample(raw_word, timestamp_ms);
	tinycardia_ble_ecg_sample(ecg_decode_raw_sample(raw_word), timestamp_ms,
				    processing_preserved);
}

static void lead_status_handler(enum max30003_lead_status status, void *user_data)
{
	enum tinycardia_lead_status protocol_status;

	ARG_UNUSED(user_data);

	switch (status) {
	case MAX30003_LEAD_STATUS_GOOD:
		protocol_status = TINYCARDIA_LEAD_STATUS_GOOD;
		break;
	case MAX30003_LEAD_STATUS_NEGATIVE_OFF:
		/* MAX30003 ECGN is protocol lead/contact 1. */
		protocol_status = TINYCARDIA_LEAD_STATUS_LEAD_1_OFF;
		break;
	case MAX30003_LEAD_STATUS_POSITIVE_OFF:
		/* MAX30003 ECGP is protocol lead/contact 2. */
		protocol_status = TINYCARDIA_LEAD_STATUS_LEAD_2_OFF;
		break;
	case MAX30003_LEAD_STATUS_BOTH_OFF:
		protocol_status = TINYCARDIA_LEAD_STATUS_BOTH_OFF;
		break;
	case MAX30003_LEAD_STATUS_UNKNOWN:
	default:
		protocol_status = TINYCARDIA_LEAD_STATUS_UNKNOWN;
		break;
	}

	(void)tinycardia_ble_status_set_lead(protocol_status);
}

static void dropped_sample_handler(uint32_t minimum_dropped, void *user_data)
{
	ARG_UNUSED(user_data);
	tinycardia_ble_record_dropped_samples(minimum_dropped);
}

static int set_monitoring(bool enabled, void *user_data)
{
	int err;

	ARG_UNUSED(user_data);

	if (!enabled) {
		err = max30003_set_monitoring(false);
		if (err < 0) {
			(void)ecg_processor_set_monitoring(max30003_is_monitoring());
			return err;
		}
		err = ecg_processor_set_monitoring(false);
		if (err == 0) {
			(void)tinycardia_ble_status_set_lead(
				TINYCARDIA_LEAD_STATUS_UNKNOWN);
		}
		return err;
	}

	(void)tinycardia_ble_status_set_lead(TINYCARDIA_LEAD_STATUS_CHECKING);
	err = ecg_processor_set_monitoring(true);
	if (err < 0) {
		return err;
	}
	err = max30003_set_monitoring(true);
	if (err < 0) {
		(void)ecg_processor_set_monitoring(max30003_is_monitoring());
		return err;
	}

	return 0;
}

static const struct tinycardia_ble_callbacks ble_callbacks = {
	.set_monitoring = set_monitoring,
};

int main(void)
{
	int err;

	err = power_control_wait_for_on();
	printk("power_control_wait_for_on returned %d\n", err);
	if (err < 0) {
		printk("Power-button initialization failed (err %d)\n", err);
		return 0;
	}

	printk("Tinycardia v2 booted\n");

	err = power_control_start_off_monitor();
	if (err < 0) {
		printk("Runtime power-button monitor initialization failed (err %d)\n", err);
		return 0;
	}

	err = ecg_processor_init(prepared_window_handler, NULL);
	if (err < 0) {
		printk("ECG processor initialization failed (err %d)\n", err);
		return 0;
	}

	err = max30003_init(live_ecg_sample_handler, NULL);
	if (err < 0) {
		printk("MAX30003 initialization failed (err %d)\n", err);
		return 0;
	}

	err = tinycardia_ble_init(&ble_callbacks, NULL, true);
	if (err < 0) {
		printk("Bluetooth initialization failed (err %d)\n", err);
		return 0;
	}
	max30003_set_lead_status_handler(lead_status_handler, NULL);
	max30003_set_drop_handler(dropped_sample_handler, NULL);

	return 0;
}
