/* SPDX-License-Identifier: MIT */

#ifndef TINYCARDIA_BLE_PROTOCOL_H_
#define TINYCARDIA_BLE_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TINYCARDIA_PROTOCOL_VERSION 0x01U

#define TINYCARDIA_ECG_MAX_SAMPLES        10U
#define TINYCARDIA_ECG_HEADER_SIZE        10U
#define TINYCARDIA_ECG_SAMPLE_SIZE        4U
#define TINYCARDIA_ECG_FULL_PACKET_SIZE   50U
#define TINYCARDIA_INFERENCE_PACKET_SIZE  13U
#define TINYCARDIA_STATUS_PACKET_SIZE     19U
#define TINYCARDIA_ATT_NOTIFICATION_OVERHEAD 3U
#define TINYCARDIA_ECG_FULL_PACKET_ATT_MTU    53U
#define TINYCARDIA_CONFIDENCE_UNAVAILABLE     UINT16_MAX

enum tinycardia_classification {
	TINYCARDIA_CLASSIFICATION_NORMAL = 0x00,
	TINYCARDIA_CLASSIFICATION_AFIB = 0x01,
	TINYCARDIA_CLASSIFICATION_UNKNOWN = 0x02,
};

enum tinycardia_signal_quality {
	TINYCARDIA_SIGNAL_QUALITY_GOOD = 0x00,
	TINYCARDIA_SIGNAL_QUALITY_POOR = 0x01,
	TINYCARDIA_SIGNAL_QUALITY_LEAD_OFF = 0x02,
	TINYCARDIA_SIGNAL_QUALITY_UNKNOWN = 0x03,
};

enum tinycardia_lead_status {
	TINYCARDIA_LEAD_STATUS_GOOD = 0x00,
	TINYCARDIA_LEAD_STATUS_LEAD_1_OFF = 0x01,
	TINYCARDIA_LEAD_STATUS_LEAD_2_OFF = 0x02,
	TINYCARDIA_LEAD_STATUS_BOTH_OFF = 0x03,
	TINYCARDIA_LEAD_STATUS_CHECKING = 0x04,
	TINYCARDIA_LEAD_STATUS_UNKNOWN = 0x05,
};

enum tinycardia_operating_state {
	TINYCARDIA_OPERATING_STATE_IDLE = 0x00,
	TINYCARDIA_OPERATING_STATE_MONITORING = 0x01,
	TINYCARDIA_OPERATING_STATE_MONITORING_AND_STREAMING = 0x02,
	TINYCARDIA_OPERATING_STATE_ERROR = 0x03,
};

enum tinycardia_control_command {
	TINYCARDIA_CONTROL_START_STREAM = 0x01,
	TINYCARDIA_CONTROL_STOP_STREAM = 0x02,
	TINYCARDIA_CONTROL_START_MONITORING = 0x03,
	TINYCARDIA_CONTROL_STOP_MONITORING = 0x04,
};

struct tinycardia_inference_result {
	uint32_t inference_id;
	uint32_t timestamp_ms;
	enum tinycardia_classification classification;
	enum tinycardia_signal_quality signal_quality;
	uint16_t confidence;
};

struct tinycardia_status {
	uint32_t uptime_s;
	uint32_t samples_acquired;
	uint32_t samples_dropped;
	uint32_t inference_count;
	enum tinycardia_lead_status lead_status;
	enum tinycardia_operating_state operating_state;
};

struct tinycardia_protocol_state {
	bool monitoring;
	bool streaming;
	bool error;
};

struct tinycardia_transport_state {
	bool connected;
	bool ecg_subscribed;
	uint8_t max_ecg_samples;
};

/** Serialize an ECG packet without relying on native structure layout. */
int tinycardia_encode_ecg_packet(uint8_t *buffer, size_t capacity,
				 uint32_t sequence, uint32_t timestamp_ms,
				 const int32_t *samples, uint8_t sample_count,
				 size_t *encoded_size);

/** Serialize one completed inference result. */
int tinycardia_encode_inference_packet(uint8_t *buffer, size_t capacity,
				       const struct tinycardia_inference_result *result);

/** Serialize the current readable/notifiable device status. */
int tinycardia_encode_status_packet(uint8_t *buffer, size_t capacity,
				    const struct tinycardia_status *status);

/** Return the number of int32 ECG samples that fit in one notification. */
uint8_t tinycardia_ecg_samples_for_att_mtu(uint16_t att_mtu);

/** True when a wrapping uptime timestamp belongs to the current session. */
bool tinycardia_timestamp_is_in_session(uint32_t timestamp_ms,
					uint32_t session_start_ms);

/** Validate and decode the exact one-byte Device Control payload. */
int tinycardia_decode_control(const uint8_t *buffer, size_t length,
			       enum tinycardia_control_command *command);

/** Apply one validated control command to the protocol state. */
int tinycardia_apply_control(struct tinycardia_protocol_state *state,
			     enum tinycardia_control_command command,
			     const struct tinycardia_transport_state *transport);

/** A disconnect stops connection streaming but leaves monitoring unchanged. */
void tinycardia_state_on_disconnect(struct tinycardia_protocol_state *state);

enum tinycardia_operating_state
tinycardia_get_operating_state(const struct tinycardia_protocol_state *state);

#endif /* TINYCARDIA_BLE_PROTOCOL_H_ */
