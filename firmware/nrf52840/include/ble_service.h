/* SPDX-License-Identifier: MIT */

#ifndef TINYCARDIA_BLE_SERVICE_H_
#define TINYCARDIA_BLE_SERVICE_H_

#include "ble_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/uuid.h>

/*
 * Tinycardia vendor UUID family (fixed for protocol v1):
 *   f8a50001-7c5b-4e91-a6d2-3b1c9e4f5200  service
 *   f8a50002-7c5b-4e91-a6d2-3b1c9e4f5200  ECG Stream
 *   f8a50003-7c5b-4e91-a6d2-3b1c9e4f5200  Inference Result
 *   f8a50004-7c5b-4e91-a6d2-3b1c9e4f5200  Device Status
 *   f8a50005-7c5b-4e91-a6d2-3b1c9e4f5200  Device Control
 */
#define TINYCARDIA_UUID_SERVICE_VAL \
	BT_UUID_128_ENCODE(0xf8a50001, 0x7c5b, 0x4e91, 0xa6d2, 0x3b1c9e4f5200)
#define TINYCARDIA_UUID_ECG_STREAM_VAL \
	BT_UUID_128_ENCODE(0xf8a50002, 0x7c5b, 0x4e91, 0xa6d2, 0x3b1c9e4f5200)
#define TINYCARDIA_UUID_INFERENCE_VAL \
	BT_UUID_128_ENCODE(0xf8a50003, 0x7c5b, 0x4e91, 0xa6d2, 0x3b1c9e4f5200)
#define TINYCARDIA_UUID_STATUS_VAL \
	BT_UUID_128_ENCODE(0xf8a50004, 0x7c5b, 0x4e91, 0xa6d2, 0x3b1c9e4f5200)
#define TINYCARDIA_UUID_CONTROL_VAL \
	BT_UUID_128_ENCODE(0xf8a50005, 0x7c5b, 0x4e91, 0xa6d2, 0x3b1c9e4f5200)

struct tinycardia_ble_callbacks {
	int (*set_monitoring)(bool enabled, void *user_data);
};

/** Enable Bluetooth, register callbacks, and begin Tinycardia advertising. */
int tinycardia_ble_init(const struct tinycardia_ble_callbacks *callbacks,
			void *user_data, bool monitoring_enabled);

/** Current logical monitoring state, independent of BLE connection state. */
bool tinycardia_ble_is_monitoring(void);

/**
 * Record and optionally queue one acquired signed MAX30003 sample.
 *
 * This function is non-blocking and is called from the MAX30003 work handler,
 * never from its GPIO ISR. processing_preserved is false when the analysis
 * path could not retain the sample; that and BLE queue failures contribute to
 * samples_dropped exactly once for this sample.
 */
void tinycardia_ble_ecg_sample(int32_t sample, uint32_t timestamp_ms,
				       bool processing_preserved);

/** Record samples lost below the application acquisition callback. */
void tinycardia_ble_record_dropped_samples(uint32_t count);

/** Publish one real completed inference result; no result is fabricated here. */
int tinycardia_ble_inference_publish(uint32_t timestamp_ms,
				     enum tinycardia_classification classification,
				     enum tinycardia_signal_quality signal_quality,
				     uint16_t confidence);

/** Update the standard Battery Level value and notify subscribers on change. */
int tinycardia_ble_battery_set_level(uint8_t percentage);

/** Update lead/contact status and notify Device Status subscribers on change. */
int tinycardia_ble_status_set_lead(enum tinycardia_lead_status lead_status);

/** Set or clear the explicit ERROR operating state. */
void tinycardia_ble_status_set_error(bool error);

#endif /* TINYCARDIA_BLE_SERVICE_H_ */
