#include "inference_policy.h"
#include "model_inference.h"

#include <errno.h>
#include <math.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#define FLOAT_TOLERANCE 1.0e-6f

static float ecg_input[TINYCARDIA_MODEL_ECG_COUNT];
static float rr_input[TINYCARDIA_MODEL_RR_COUNT];

ZTEST(inference_policy, test_rejects_invalid_or_low_quality_windows)
{
	const uint32_t window_start_ms = 10000U;
	const uint32_t good_before_window_ms = 9000U;

	zassert_true(tinycardia_inference_window_is_eligible(
		true, TINYCARDIA_SIGNAL_QUALITY_GOOD, window_start_ms, good_before_window_ms));
	zassert_false(tinycardia_inference_window_is_eligible(
		false, TINYCARDIA_SIGNAL_QUALITY_GOOD, window_start_ms, good_before_window_ms));
	zassert_false(tinycardia_inference_window_is_eligible(
		true, TINYCARDIA_SIGNAL_QUALITY_POOR, window_start_ms, good_before_window_ms));
	zassert_false(tinycardia_inference_window_is_eligible(
		true, TINYCARDIA_SIGNAL_QUALITY_LEAD_OFF, window_start_ms, good_before_window_ms));
	zassert_false(tinycardia_inference_window_is_eligible(
		true, TINYCARDIA_SIGNAL_QUALITY_UNKNOWN, window_start_ms, good_before_window_ms));
	/* Contact recovered after capture began: this window is contaminated. */
	zassert_false(tinycardia_inference_window_is_eligible(true, TINYCARDIA_SIGNAL_QUALITY_GOOD,
							      window_start_ms, 15000U));
	/* Contact may become good at the exact capture boundary. */
	zassert_true(tinycardia_inference_window_is_eligible(true, TINYCARDIA_SIGNAL_QUALITY_GOOD,
							     window_start_ms, window_start_ms));
	/* Expanded timestamp ordering remains valid across the 32-bit wire wrap. */
	zassert_true(tinycardia_inference_window_is_eligible(true, TINYCARDIA_SIGNAL_QUALITY_GOOD,
							     0x100000100ULL, 0x0fffffff0ULL));
	zassert_false(tinycardia_inference_window_is_eligible(true, TINYCARDIA_SIGNAL_QUALITY_GOOD,
							      0x0fffffff0ULL, 0x100000100ULL));
}

ZTEST(model_quantization, test_zero_positive_negative_and_saturation)
{
	zassert_equal(tinycardia_model_quantize(0.0f, TINYCARDIA_MODEL_ECG_SCALE,
						TINYCARDIA_MODEL_ECG_ZERO_POINT),
		      -15);
	zassert_equal(tinycardia_model_quantize(TINYCARDIA_MODEL_ECG_SCALE,
						TINYCARDIA_MODEL_ECG_SCALE,
						TINYCARDIA_MODEL_ECG_ZERO_POINT),
		      -14);
	zassert_equal(tinycardia_model_quantize(-TINYCARDIA_MODEL_ECG_SCALE,
						TINYCARDIA_MODEL_ECG_SCALE,
						TINYCARDIA_MODEL_ECG_ZERO_POINT),
		      -16);
	zassert_equal(tinycardia_model_quantize(1000.0f, TINYCARDIA_MODEL_ECG_SCALE,
						TINYCARDIA_MODEL_ECG_ZERO_POINT),
		      INT8_MAX);
	zassert_equal(tinycardia_model_quantize(-1000.0f, TINYCARDIA_MODEL_ECG_SCALE,
						TINYCARDIA_MODEL_ECG_ZERO_POINT),
		      INT8_MIN);

	zassert_equal(tinycardia_model_quantize(0.0f, TINYCARDIA_MODEL_RR_SCALE,
						TINYCARDIA_MODEL_RR_ZERO_POINT),
		      -27);
	zassert_equal(tinycardia_model_quantize(TINYCARDIA_MODEL_RR_SCALE,
						TINYCARDIA_MODEL_RR_SCALE,
						TINYCARDIA_MODEL_RR_ZERO_POINT),
		      -26);
	zassert_equal(tinycardia_model_quantize(-TINYCARDIA_MODEL_RR_SCALE,
						TINYCARDIA_MODEL_RR_SCALE,
						TINYCARDIA_MODEL_RR_ZERO_POINT),
		      -28);
	zassert_equal(tinycardia_model_quantize(1000.0f, TINYCARDIA_MODEL_RR_SCALE,
						TINYCARDIA_MODEL_RR_ZERO_POINT),
		      INT8_MAX);
	zassert_equal(tinycardia_model_quantize(-1000.0f, TINYCARDIA_MODEL_RR_SCALE,
						TINYCARDIA_MODEL_RR_ZERO_POINT),
		      INT8_MIN);
}

ZTEST(model_quantization, test_known_partial_reference_round_trips_exactly)
{
	static const int8_t known_ecg[] = {
		-19, -33, -28, -30, -27, -27, -24, -26, -35, -37,
	};
	static const int8_t known_rr[] = {-18, -60, -67, -89, -23, -72, -45};

	for (size_t index = 0U; index < ARRAY_SIZE(known_ecg); ++index) {
		float value =
			tinycardia_model_dequantize(known_ecg[index], TINYCARDIA_MODEL_ECG_SCALE,
						    TINYCARDIA_MODEL_ECG_ZERO_POINT);

		zassert_equal(tinycardia_model_quantize(value, TINYCARDIA_MODEL_ECG_SCALE,
							TINYCARDIA_MODEL_ECG_ZERO_POINT),
			      known_ecg[index]);
	}
	for (size_t index = 0U; index < ARRAY_SIZE(known_rr); ++index) {
		float value = tinycardia_model_dequantize(
			known_rr[index], TINYCARDIA_MODEL_RR_SCALE, TINYCARDIA_MODEL_RR_ZERO_POINT);

		zassert_equal(tinycardia_model_quantize(value, TINYCARDIA_MODEL_RR_SCALE,
							TINYCARDIA_MODEL_RR_ZERO_POINT),
			      known_rr[index]);
	}
}

ZTEST(model_quantization, test_output_dequantization_reference)
{
	zassert_within(tinycardia_model_dequantize(INT8_MIN, TINYCARDIA_MODEL_OUTPUT_SCALE,
						   TINYCARDIA_MODEL_OUTPUT_ZERO_POINT),
		       0.0f, FLOAT_TOLERANCE);
	zassert_within(tinycardia_model_dequantize(INT8_MAX, TINYCARDIA_MODEL_OUTPUT_SCALE,
						   TINYCARDIA_MODEL_OUTPUT_ZERO_POINT),
		       0.99609375f, FLOAT_TOLERANCE);
}

ZTEST(model_runtime, test_metadata_allocation_and_real_invoke)
{
	struct tinycardia_model_result result;
	float probability_sum;
	float selected_probability;
	long expected_confidence;

	zassert_ok(tinycardia_model_init(),
		   "init validates schema, graph, names, shapes, types and quantization");
	zassert_true(tinycardia_model_arena_used_bytes() > 0U);
	zassert_equal(tinycardia_model_init(), -EALREADY);
	zassert_equal(tinycardia_model_infer(ecg_input, ARRAY_SIZE(ecg_input) - 1U, rr_input,
					     ARRAY_SIZE(rr_input), &result),
		      -EMSGSIZE);

	zassert_ok(tinycardia_model_infer(ecg_input, ARRAY_SIZE(ecg_input), rr_input,
					  ARRAY_SIZE(rr_input), &result));
	/*
	 * Independent LiteRT 2.2.0 oracle for real-valued all-zero inputs against
	 * canonical model SHA-256 85a9a574...20c50b6.
	 */
	zassert_equal(result.quantized_probabilities[0], -49);
	zassert_equal(result.quantized_probabilities[1], 49);
	zassert_equal(result.classification, TINYCARDIA_CLASSIFICATION_NORMAL);
	zassert_equal(result.confidence, 6914U);
	probability_sum = tinycardia_model_dequantize(result.quantized_probabilities[0],
						      TINYCARDIA_MODEL_OUTPUT_SCALE,
						      TINYCARDIA_MODEL_OUTPUT_ZERO_POINT) +
			  tinycardia_model_dequantize(result.quantized_probabilities[1],
						      TINYCARDIA_MODEL_OUTPUT_SCALE,
						      TINYCARDIA_MODEL_OUTPUT_ZERO_POINT);
	zassert_within(probability_sum, 1.0f, TINYCARDIA_MODEL_OUTPUT_SCALE);

	if (result.quantized_probabilities[0] >= result.quantized_probabilities[1]) {
		zassert_equal(result.classification, TINYCARDIA_CLASSIFICATION_AFIB);
		selected_probability = tinycardia_model_dequantize(
			result.quantized_probabilities[0], TINYCARDIA_MODEL_OUTPUT_SCALE,
			TINYCARDIA_MODEL_OUTPUT_ZERO_POINT);
	} else {
		zassert_equal(result.classification, TINYCARDIA_CLASSIFICATION_NORMAL);
		selected_probability = tinycardia_model_dequantize(
			result.quantized_probabilities[1], TINYCARDIA_MODEL_OUTPUT_SCALE,
			TINYCARDIA_MODEL_OUTPUT_ZERO_POINT);
	}
	expected_confidence = lroundf(selected_probability * 10000.0f);
	zassert_equal(result.confidence, expected_confidence);
}

ZTEST_SUITE(model_quantization, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(model_runtime, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(inference_policy, NULL, NULL, NULL, NULL, NULL);
