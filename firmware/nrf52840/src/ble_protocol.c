/* SPDX-License-Identifier: MIT */

#include "ble_protocol.h"

#include <errno.h>

#include <zephyr/sys/byteorder.h>

static bool inference_result_is_valid(const struct tinycardia_inference_result *result)
{
	return result->classification >= TINYCARDIA_CLASSIFICATION_NORMAL &&
	       result->classification <= TINYCARDIA_CLASSIFICATION_UNKNOWN &&
	       result->signal_quality >= TINYCARDIA_SIGNAL_QUALITY_GOOD &&
	       result->signal_quality <= TINYCARDIA_SIGNAL_QUALITY_UNKNOWN &&
	       (result->confidence <= 10000U ||
		result->confidence == TINYCARDIA_CONFIDENCE_UNAVAILABLE);
}

int tinycardia_encode_ecg_packet(uint8_t *buffer, size_t capacity,
				 uint32_t sequence, uint32_t timestamp_ms,
				 const int32_t *samples, uint8_t sample_count,
				 size_t *encoded_size)
{
	size_t packet_size;

	if (buffer == NULL || samples == NULL || encoded_size == NULL || sample_count == 0U ||
	    sample_count > TINYCARDIA_ECG_MAX_SAMPLES) {
		return -EINVAL;
	}

	packet_size = TINYCARDIA_ECG_HEADER_SIZE +
		      (size_t)sample_count * TINYCARDIA_ECG_SAMPLE_SIZE;
	if (capacity < packet_size) {
		return -EMSGSIZE;
	}

	buffer[0] = TINYCARDIA_PROTOCOL_VERSION;
	sys_put_le32(sequence, &buffer[1]);
	sys_put_le32(timestamp_ms, &buffer[5]);
	buffer[9] = sample_count;
	for (size_t index = 0; index < sample_count; ++index) {
		sys_put_le32((uint32_t)samples[index],
			     &buffer[TINYCARDIA_ECG_HEADER_SIZE +
				     index * TINYCARDIA_ECG_SAMPLE_SIZE]);
	}

	*encoded_size = packet_size;
	return 0;
}

int tinycardia_encode_inference_packet(uint8_t *buffer, size_t capacity,
				       const struct tinycardia_inference_result *result)
{
	if (buffer == NULL || result == NULL || !inference_result_is_valid(result)) {
		return -EINVAL;
	}
	if (capacity < TINYCARDIA_INFERENCE_PACKET_SIZE) {
		return -EMSGSIZE;
	}

	buffer[0] = TINYCARDIA_PROTOCOL_VERSION;
	sys_put_le32(result->inference_id, &buffer[1]);
	sys_put_le32(result->timestamp_ms, &buffer[5]);
	buffer[9] = (uint8_t)result->classification;
	buffer[10] = (uint8_t)result->signal_quality;
	sys_put_le16(result->confidence, &buffer[11]);

	return 0;
}

int tinycardia_encode_status_packet(uint8_t *buffer, size_t capacity,
				    const struct tinycardia_status *status)
{
	if (buffer == NULL || status == NULL ||
	    status->lead_status < TINYCARDIA_LEAD_STATUS_GOOD ||
	    status->lead_status > TINYCARDIA_LEAD_STATUS_UNKNOWN ||
	    status->operating_state < TINYCARDIA_OPERATING_STATE_IDLE ||
	    status->operating_state > TINYCARDIA_OPERATING_STATE_ERROR) {
		return -EINVAL;
	}
	if (capacity < TINYCARDIA_STATUS_PACKET_SIZE) {
		return -EMSGSIZE;
	}

	buffer[0] = TINYCARDIA_PROTOCOL_VERSION;
	sys_put_le32(status->uptime_s, &buffer[1]);
	sys_put_le32(status->samples_acquired, &buffer[5]);
	sys_put_le32(status->samples_dropped, &buffer[9]);
	sys_put_le32(status->inference_count, &buffer[13]);
	buffer[17] = (uint8_t)status->lead_status;
	buffer[18] = (uint8_t)status->operating_state;

	return 0;
}

uint8_t tinycardia_ecg_samples_for_att_mtu(uint16_t att_mtu)
{
	size_t value_capacity;
	size_t samples;

	if (att_mtu <= TINYCARDIA_ATT_NOTIFICATION_OVERHEAD) {
		return 0U;
	}

	value_capacity = att_mtu - TINYCARDIA_ATT_NOTIFICATION_OVERHEAD;
	if (value_capacity < TINYCARDIA_ECG_HEADER_SIZE + TINYCARDIA_ECG_SAMPLE_SIZE) {
		return 0U;
	}

	samples = (value_capacity - TINYCARDIA_ECG_HEADER_SIZE) /
		  TINYCARDIA_ECG_SAMPLE_SIZE;
	if (samples > TINYCARDIA_ECG_MAX_SAMPLES) {
		samples = TINYCARDIA_ECG_MAX_SAMPLES;
	}

	return (uint8_t)samples;
}

bool tinycardia_timestamp_is_in_session(uint32_t timestamp_ms,
					uint32_t session_start_ms)
{
	/* Valid for comparisons separated by less than half the uint32_t range. */
	return (int32_t)(timestamp_ms - session_start_ms) >= 0;
}

int tinycardia_decode_control(const uint8_t *buffer, size_t length,
			       enum tinycardia_control_command *command)
{
	if (buffer == NULL || command == NULL) {
		return -EINVAL;
	}
	if (length != 1U) {
		return -EMSGSIZE;
	}
	if (buffer[0] < TINYCARDIA_CONTROL_START_STREAM ||
	    buffer[0] > TINYCARDIA_CONTROL_STOP_MONITORING) {
		return -ENOTSUP;
	}

	*command = (enum tinycardia_control_command)buffer[0];
	return 0;
}

int tinycardia_apply_control(struct tinycardia_protocol_state *state,
			     enum tinycardia_control_command command,
			     const struct tinycardia_transport_state *transport)
{
	if (state == NULL || transport == NULL) {
		return -EINVAL;
	}

	switch (command) {
	case TINYCARDIA_CONTROL_START_STREAM:
		if (!state->monitoring) {
			return -EACCES;
		}
		if (!transport->connected || !transport->ecg_subscribed ||
		    transport->max_ecg_samples == 0U) {
			return -EAGAIN;
		}
		state->streaming = true;
		return 0;
	case TINYCARDIA_CONTROL_STOP_STREAM:
		state->streaming = false;
		return 0;
	case TINYCARDIA_CONTROL_START_MONITORING:
		state->monitoring = true;
		return 0;
	case TINYCARDIA_CONTROL_STOP_MONITORING:
		state->streaming = false;
		state->monitoring = false;
		return 0;
	default:
		return -ENOTSUP;
	}
}

void tinycardia_state_on_disconnect(struct tinycardia_protocol_state *state)
{
	if (state != NULL) {
		state->streaming = false;
	}
}

enum tinycardia_operating_state
tinycardia_get_operating_state(const struct tinycardia_protocol_state *state)
{
	if (state == NULL || state->error) {
		return TINYCARDIA_OPERATING_STATE_ERROR;
	}
	if (state->monitoring && state->streaming) {
		return TINYCARDIA_OPERATING_STATE_MONITORING_AND_STREAMING;
	}
	if (state->monitoring) {
		return TINYCARDIA_OPERATING_STATE_MONITORING;
	}

	return TINYCARDIA_OPERATING_STATE_IDLE;
}
