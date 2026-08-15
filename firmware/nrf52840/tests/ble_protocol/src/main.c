/* SPDX-License-Identifier: MIT */

#include "ble_protocol.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

ZTEST(ble_ecg_packet, test_full_packet_is_exact_and_little_endian)
{
	static const int32_t samples[TINYCARDIA_ECG_MAX_SAMPLES] = {
		0x12345678, -1, INT32_MIN, INT32_MAX, 0, 1, -2, 42, -74565, 74565,
	};
	uint8_t packet[TINYCARDIA_ECG_FULL_PACKET_SIZE];
	size_t packet_size = 0U;

	zassert_ok(tinycardia_encode_ecg_packet(packet, sizeof(packet), 0x12345678U,
						0x90abcdefU, samples, ARRAY_SIZE(samples),
						&packet_size));
	zassert_equal(packet_size, TINYCARDIA_ECG_FULL_PACKET_SIZE);
	zassert_equal(packet[0], TINYCARDIA_PROTOCOL_VERSION);
	zassert_mem_equal(&packet[1], ((uint8_t[]){ 0x78, 0x56, 0x34, 0x12 }), 4U);
	zassert_mem_equal(&packet[5], ((uint8_t[]){ 0xef, 0xcd, 0xab, 0x90 }), 4U);
	zassert_equal(packet[9], TINYCARDIA_ECG_MAX_SAMPLES);
	zassert_mem_equal(&packet[10], ((uint8_t[]){ 0x78, 0x56, 0x34, 0x12 }), 4U);
	zassert_mem_equal(&packet[14], ((uint8_t[]){ 0xff, 0xff, 0xff, 0xff }), 4U);
	zassert_mem_equal(&packet[18], ((uint8_t[]){ 0x00, 0x00, 0x00, 0x80 }), 4U);
	zassert_mem_equal(&packet[22], ((uint8_t[]){ 0xff, 0xff, 0xff, 0x7f }), 4U);
}

ZTEST(ble_ecg_packet, test_short_packet_and_boundaries)
{
	static const int32_t samples[] = { -74565, 74565 };
	uint8_t packet[TINYCARDIA_ECG_FULL_PACKET_SIZE];
	size_t packet_size = 0U;

	zassert_ok(tinycardia_encode_ecg_packet(packet, sizeof(packet), 0U, 4U, samples,
						ARRAY_SIZE(samples), &packet_size));
	zassert_equal(packet_size, 18U);
	zassert_equal(packet[9], 2U);
	zassert_mem_equal(&packet[10], ((uint8_t[]){ 0xbb, 0xdc, 0xfe, 0xff }), 4U);
	zassert_mem_equal(&packet[14], ((uint8_t[]){ 0x45, 0x23, 0x01, 0x00 }), 4U);
	zassert_equal(tinycardia_encode_ecg_packet(packet, 17U, 0U, 0U, samples, 2U,
						   &packet_size),
		      -EMSGSIZE);
	zassert_equal(tinycardia_encode_ecg_packet(packet, sizeof(packet), 0U, 0U, samples,
						   0U, &packet_size),
		      -EINVAL);
	zassert_equal(tinycardia_encode_ecg_packet(packet, sizeof(packet), 0U, 0U, samples,
						   TINYCARDIA_ECG_MAX_SAMPLES + 1U,
						   &packet_size),
		      -EINVAL);
}

ZTEST(ble_ecg_packet, test_mtu_selects_largest_unfragmented_sample_count)
{
	zassert_equal(tinycardia_ecg_samples_for_att_mtu(13U), 0U);
	zassert_equal(tinycardia_ecg_samples_for_att_mtu(17U), 1U);
	zassert_equal(tinycardia_ecg_samples_for_att_mtu(23U), 2U);
	zassert_equal(tinycardia_ecg_samples_for_att_mtu(52U), 9U);
	zassert_equal(tinycardia_ecg_samples_for_att_mtu(53U), 10U);
	zassert_equal(tinycardia_ecg_samples_for_att_mtu(247U), 10U);
}

ZTEST(ble_ecg_packet, test_session_timestamp_rejects_stale_results_across_wrap)
{
	zassert_false(tinycardia_timestamp_is_in_session(99U, 100U));
	zassert_true(tinycardia_timestamp_is_in_session(100U, 100U));
	zassert_true(tinycardia_timestamp_is_in_session(101U, 100U));
	zassert_true(tinycardia_timestamp_is_in_session(0x00000010U, 0xfffffff0U));
	zassert_false(tinycardia_timestamp_is_in_session(0xffffff00U, 0x00000010U));
}

ZTEST(ble_inference_packet, test_exact_layout_and_confidence_values)
{
	struct tinycardia_inference_result result = {
		.inference_id = 0x12345678U,
		.timestamp_ms = 0x90abcdefU,
		.classification = TINYCARDIA_CLASSIFICATION_AFIB,
		.signal_quality = TINYCARDIA_SIGNAL_QUALITY_POOR,
		.confidence = 10000U,
	};
	uint8_t packet[TINYCARDIA_INFERENCE_PACKET_SIZE];

	zassert_ok(tinycardia_encode_inference_packet(packet, sizeof(packet), &result));
	zassert_equal(sizeof(packet), 13U);
	zassert_equal(packet[0], 0x01U);
	zassert_mem_equal(&packet[1], ((uint8_t[]){ 0x78, 0x56, 0x34, 0x12 }), 4U);
	zassert_mem_equal(&packet[5], ((uint8_t[]){ 0xef, 0xcd, 0xab, 0x90 }), 4U);
	zassert_equal(packet[9], TINYCARDIA_CLASSIFICATION_AFIB);
	zassert_equal(packet[10], TINYCARDIA_SIGNAL_QUALITY_POOR);
	zassert_mem_equal(&packet[11], ((uint8_t[]){ 0x10, 0x27 }), 2U);

	result.confidence = TINYCARDIA_CONFIDENCE_UNAVAILABLE;
	zassert_ok(tinycardia_encode_inference_packet(packet, sizeof(packet), &result));
	zassert_mem_equal(&packet[11], ((uint8_t[]){ 0xff, 0xff }), 2U);
	result.confidence = 10001U;
	zassert_equal(tinycardia_encode_inference_packet(packet, sizeof(packet), &result),
		      -EINVAL);
	result.confidence = 10000U;
	result.classification = (enum tinycardia_classification)-1;
	zassert_equal(tinycardia_encode_inference_packet(packet, sizeof(packet), &result),
		      -EINVAL);
	result.classification = TINYCARDIA_CLASSIFICATION_NORMAL;
	result.signal_quality = (enum tinycardia_signal_quality)-1;
	zassert_equal(tinycardia_encode_inference_packet(packet, sizeof(packet), &result),
		      -EINVAL);
}

ZTEST(ble_status_packet, test_exact_layout_and_all_counters)
{
	const struct tinycardia_status status = {
		.uptime_s = 0x12345678U,
		.samples_acquired = 0x90abcdefU,
		.samples_dropped = 0x01020304U,
		.inference_count = 0xfedcba98U,
		.lead_status = TINYCARDIA_LEAD_STATUS_BOTH_OFF,
		.operating_state = TINYCARDIA_OPERATING_STATE_MONITORING_AND_STREAMING,
	};
	struct tinycardia_status invalid;
	uint8_t packet[TINYCARDIA_STATUS_PACKET_SIZE];

	zassert_ok(tinycardia_encode_status_packet(packet, sizeof(packet), &status));
	zassert_equal(sizeof(packet), 19U);
	zassert_mem_equal(packet,
			  ((uint8_t[]){ 0x01, 0x78, 0x56, 0x34, 0x12, 0xef, 0xcd,
					 0xab, 0x90, 0x04, 0x03, 0x02, 0x01, 0x98,
					 0xba, 0xdc, 0xfe, 0x03, 0x02 }),
			  sizeof(packet));

	invalid = status;
	invalid.lead_status = (enum tinycardia_lead_status)-1;
	zassert_equal(tinycardia_encode_status_packet(packet, sizeof(packet), &invalid),
		      -EINVAL);
	invalid = status;
	invalid.operating_state = (enum tinycardia_operating_state)-1;
	zassert_equal(tinycardia_encode_status_packet(packet, sizeof(packet), &invalid),
		      -EINVAL);
}

ZTEST(ble_control, test_valid_and_invalid_control_payloads)
{
	enum tinycardia_control_command command;
	uint8_t payload[2] = { TINYCARDIA_CONTROL_START_STREAM, 0U };

	for (uint8_t value = TINYCARDIA_CONTROL_START_STREAM;
	     value <= TINYCARDIA_CONTROL_STOP_MONITORING; ++value) {
		payload[0] = value;
		zassert_ok(tinycardia_decode_control(payload, 1U, &command));
		zassert_equal(command, value);
	}

	payload[0] = 0U;
	zassert_equal(tinycardia_decode_control(payload, 1U, &command), -ENOTSUP);
	payload[0] = 5U;
	zassert_equal(tinycardia_decode_control(payload, 1U, &command), -ENOTSUP);
	zassert_equal(tinycardia_decode_control(payload, 0U, &command), -EMSGSIZE);
	zassert_equal(tinycardia_decode_control(payload, 2U, &command), -EMSGSIZE);
}

ZTEST(ble_state, test_monitoring_streaming_and_disconnect_transitions)
{
	struct tinycardia_protocol_state state = { 0 };
	const struct tinycardia_transport_state unavailable = { 0 };
	const struct tinycardia_transport_state ready = {
		.connected = true,
		.ecg_subscribed = true,
		.max_ecg_samples = TINYCARDIA_ECG_MAX_SAMPLES,
	};

	zassert_equal(tinycardia_get_operating_state(&state),
		      TINYCARDIA_OPERATING_STATE_IDLE);
	zassert_ok(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_START_MONITORING,
					    &unavailable));
	zassert_equal(tinycardia_get_operating_state(&state),
		      TINYCARDIA_OPERATING_STATE_MONITORING);
	zassert_equal(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_START_STREAM,
					       &unavailable),
		      -EAGAIN);
	zassert_true(state.monitoring);
	zassert_false(state.streaming);

	zassert_ok(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_START_STREAM, &ready));
	zassert_equal(tinycardia_get_operating_state(&state),
		      TINYCARDIA_OPERATING_STATE_MONITORING_AND_STREAMING);
	zassert_ok(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_STOP_STREAM, &ready));
	zassert_true(state.monitoring);
	zassert_false(state.streaming);

	zassert_ok(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_START_STREAM, &ready));
	tinycardia_state_on_disconnect(&state);
	zassert_true(state.monitoring);
	zassert_false(state.streaming);

	zassert_ok(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_START_STREAM, &ready));
	zassert_ok(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_STOP_MONITORING,
					    &ready));
	zassert_false(state.monitoring);
	zassert_false(state.streaming);
	zassert_equal(tinycardia_get_operating_state(&state),
		      TINYCARDIA_OPERATING_STATE_IDLE);
	zassert_equal(tinycardia_apply_control(&state, TINYCARDIA_CONTROL_START_STREAM, &ready),
		      -EACCES);
}

ZTEST_SUITE(ble_ecg_packet, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(ble_inference_packet, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(ble_status_packet, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(ble_control, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(ble_state, NULL, NULL, NULL, NULL, NULL);
