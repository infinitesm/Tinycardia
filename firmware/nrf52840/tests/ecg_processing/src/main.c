// SPDX-License-Identifier: MIT

#include "ecg_processing.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#define CANARY_BEFORE 0x13579bdfU
#define CANARY_AFTER  0x2468ace0U

#define FLOAT_TOLERANCE       1.0e-4f
#define LARGE_FLOAT_TOLERANCE 2.0e-2f

struct guarded_window {
	uint32_t before;
	struct ecg_sample_window window;
	uint32_t after;
};

static struct guarded_window guarded_window;
static struct ecg_processing_workspace workspace;
static struct ecg_processing_result processing_result;

static void assert_float_array(const float *actual, const float *expected, size_t count,
			       float tolerance)
{
	for (size_t index = 0; index < count; ++index) {
		zassert_within(actual[index], expected[index], tolerance,
			       "float mismatch at index %u", (unsigned int)index);
	}
}

static void reset_guarded_window(void)
{
	guarded_window.before = CANARY_BEFORE;
	guarded_window.after = CANARY_AFTER;
	ecg_sample_window_reset(&guarded_window.window);
}

ZTEST(ecg_decode, test_signed_18_bit_boundaries_and_representative_values)
{
	static const struct {
		uint32_t raw_word;
		int32_t expected;
	} vectors[] = {
		{ 0x00003fU, 0 },
		{ 0x00007fU, 1 },
		{ 0x48d17fU, 74565 },
		{ 0x7fffffU, 131071 },
		{ 0xffffffU, -1 },
		{ 0xb72effU, -74565 },
		{ 0x80003fU, -131072 },
	};

	for (size_t index = 0; index < ARRAY_SIZE(vectors); ++index) {
		zassert_equal(ecg_decode_raw_sample(vectors[index].raw_word),
			      vectors[index].expected, "decode vector %u failed", (unsigned int)index);
	}
}

ZTEST(ecg_decode, test_fifo_tag_bits_do_not_change_sample)
{
	static const uint32_t tag_values[] = { 0x00U, 0x01U, 0x08U, 0x15U, 0x2aU, 0x3fU };

	for (size_t index = 0; index < ARRAY_SIZE(tag_values); ++index) {
		zassert_equal(ecg_decode_raw_sample(0x48d140U | tag_values[index]), 74565,
			      "positive sample changed with tag vector %u", (unsigned int)index);
		zassert_equal(ecg_decode_raw_sample(0xb72ec0U | tag_values[index]), -74565,
			      "negative sample changed with tag vector %u", (unsigned int)index);
	}
}

ZTEST(ecg_decode, test_counts_to_millivolts_conversion)
{
	zassert_within(ecg_decode_sample_mv(0x000040U), 0.0002479553f, 1.0e-9f);
	zassert_within(ecg_decode_sample_mv(0x7fffc0U), 32.499752f, FLOAT_TOLERANCE);
	zassert_within(ecg_decode_sample_mv(0x800000U), -32.5f, FLOAT_TOLERANCE);
}

ZTEST(ecg_window, test_partial_window_preserves_exact_sequence)
{
	reset_guarded_window();

	for (size_t index = 0; index < 17U; ++index) {
		zassert_equal(ecg_sample_window_append(&guarded_window.window, (float)index),
			      ECG_WINDOW_SAMPLE_STORED);
	}

	zassert_equal(guarded_window.window.count, 17U);
	for (size_t index = 0; index < guarded_window.window.count; ++index) {
		zassert_equal(guarded_window.window.samples[index], (float)index,
			      "partial window mismatch at %u", (unsigned int)index);
	}
	zassert_equal(guarded_window.before, CANARY_BEFORE);
	zassert_equal(guarded_window.after, CANARY_AFTER);
}

ZTEST(ecg_window, test_exact_completion_rejects_out_of_bounds_append)
{
	reset_guarded_window();

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		enum ecg_window_append_result expected =
			index == ECG_PROCESSOR_WINDOW_SIZE - 1U ? ECG_WINDOW_COMPLETED
							       : ECG_WINDOW_SAMPLE_STORED;

		zassert_equal(ecg_sample_window_append(&guarded_window.window, (float)index),
			      expected, "completion state mismatch at %u", (unsigned int)index);
	}

	zassert_equal(guarded_window.window.count, ECG_PROCESSOR_WINDOW_SIZE);
	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		zassert_equal(guarded_window.window.samples[index], (float)index,
			      "sample skipped or duplicated at %u", (unsigned int)index);
	}

	zassert_equal(ecg_sample_window_append(&guarded_window.window, 12345.0f),
		      ECG_WINDOW_ALREADY_FULL);
	zassert_equal(guarded_window.window.count, ECG_PROCESSOR_WINDOW_SIZE);
	zassert_equal(guarded_window.window.samples[ECG_PROCESSOR_WINDOW_SIZE - 1U],
		      (float)(ECG_PROCESSOR_WINDOW_SIZE - 1U));
	zassert_equal(guarded_window.before, CANARY_BEFORE);
	zassert_equal(guarded_window.after, CANARY_AFTER);
}

ZTEST(ecg_window, test_reset_produces_non_overlapping_consecutive_window)
{
	reset_guarded_window();

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		(void)ecg_sample_window_append(&guarded_window.window, (float)index);
	}
	ecg_sample_window_reset(&guarded_window.window);
	zassert_equal(guarded_window.window.count, 0U);

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		float second_window_sample = 10000.0f + (float)index;
		enum ecg_window_append_result expected =
			index == ECG_PROCESSOR_WINDOW_SIZE - 1U ? ECG_WINDOW_COMPLETED
							       : ECG_WINDOW_SAMPLE_STORED;

		zassert_equal(ecg_sample_window_append(&guarded_window.window,
						       second_window_sample),
			      expected);
	}

	for (size_t index = 0; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		zassert_equal(guarded_window.window.samples[index], 10000.0f + (float)index,
			      "stale or overlapping sample at %u", (unsigned int)index);
	}
	zassert_equal(guarded_window.before, CANARY_BEFORE);
	zassert_equal(guarded_window.after, CANARY_AFTER);
}

ZTEST(ecg_rr, test_known_peak_indices_produce_explicit_intervals_and_features)
{
	static const size_t peaks[] = { 0U, 256U, 512U, 896U };
	static const float expected_intervals[] = { 1000.0f, 1000.0f, 1500.0f };
	static const float expected_features[] = {
		1166.666667f, 288.675135f, 353.553391f, 0.5f, 0.5f, 176.776695f,
		282.597083f,
	};
	static const float expected_standardized[] = {
		1.37586599f, 1.36902791f, 0.97420778f, 0.55162963f, 0.07036942f,
		0.61352309f, 1.73517839f,
	};
	struct ecg_rr_result result;

	zassert_ok(ecg_extract_rr_features(peaks, ARRAY_SIZE(peaks), &result));
	zassert_true(result.features_valid);
	zassert_equal(result.interval_count, ARRAY_SIZE(expected_intervals));
	assert_float_array(result.intervals_ms, expected_intervals, ARRAY_SIZE(expected_intervals),
			   FLOAT_TOLERANCE);
	assert_float_array(result.features_unscaled, expected_features,
			   ARRAY_SIZE(expected_features), LARGE_FLOAT_TOLERANCE);
	assert_float_array(result.features_standardized, expected_standardized,
			   ARRAY_SIZE(expected_standardized), FLOAT_TOLERANCE);
}

ZTEST(ecg_rr, test_insufficient_peaks_report_unavailable_features)
{
	static const size_t two_peaks[] = { 100U, 356U };
	static const float expected_zero_standardized[] = {
		-6.42225338f, -0.72462300f, -0.69026889f, -0.91497965f,
		-1.46363233f, -0.81666524f, -0.62238771f,
	};
	static const float expected_zero_features[ECG_PROCESSOR_RR_FEATURE_COUNT] = { 0.0f };
	struct ecg_rr_result result;

	zassert_ok(ecg_extract_rr_features(NULL, 0U, &result));
	zassert_false(result.features_valid);
	zassert_equal(result.interval_count, 0U);
	assert_float_array(result.features_unscaled, expected_zero_features,
			   ECG_PROCESSOR_RR_FEATURE_COUNT, 0.0f);
	assert_float_array(result.features_standardized, expected_zero_standardized,
			   ECG_PROCESSOR_RR_FEATURE_COUNT, FLOAT_TOLERANCE);

	zassert_ok(ecg_extract_rr_features(two_peaks, ARRAY_SIZE(two_peaks), &result));
	zassert_false(result.features_valid);
	zassert_equal(result.interval_count, 1U);
	zassert_equal(result.intervals_ms[0], 1000.0f);
	assert_float_array(result.features_unscaled, expected_zero_features,
			   ECG_PROCESSOR_RR_FEATURE_COUNT, 0.0f);
}

ZTEST(ecg_rr, test_boundary_intervals_and_invalid_peak_lists)
{
	static const size_t boundary_peaks[] = { 0U, 1U, 2559U };
	static const float expected_intervals[] = { 3.90625f, 9992.1875f };
	static const float expected_features[] = {
		4998.046875f, 7062.781404f, 9988.28125f, 1.0f, 1.0f, 0.0f, 7062.781404f,
	};
	static const size_t duplicate_peaks[] = { 10U, 10U, 300U };
	static const size_t descending_peaks[] = { 100U, 50U, 300U };
	static const size_t out_of_range_peaks[] = { 0U, 256U, ECG_PROCESSOR_WINDOW_SIZE };
	size_t too_many_peaks[ECG_PROCESSING_MAX_R_PEAKS + 1U];
	struct ecg_rr_result result;

	zassert_ok(ecg_extract_rr_features(boundary_peaks, ARRAY_SIZE(boundary_peaks), &result));
	zassert_true(result.features_valid);
	assert_float_array(result.intervals_ms, expected_intervals, ARRAY_SIZE(expected_intervals),
			   FLOAT_TOLERANCE);
	assert_float_array(result.features_unscaled, expected_features,
			   ARRAY_SIZE(expected_features), LARGE_FLOAT_TOLERANCE);

	zassert_equal(ecg_extract_rr_features(duplicate_peaks, ARRAY_SIZE(duplicate_peaks),
					      &result),
		      -EINVAL);
	zassert_equal(ecg_extract_rr_features(descending_peaks, ARRAY_SIZE(descending_peaks),
					      &result),
		      -EINVAL);
	zassert_equal(ecg_extract_rr_features(out_of_range_peaks,
					      ARRAY_SIZE(out_of_range_peaks), &result),
		      -ERANGE);
	zassert_equal(ecg_extract_rr_features(too_many_peaks, ARRAY_SIZE(too_many_peaks),
					      &result),
		      -E2BIG);
}

ZTEST(ecg_regression, test_deterministic_ecg_golden_model_inputs)
{
	static const size_t fixture_centers[] = {
		128U, 384U, 640U, 896U, 1152U, 1408U, 1664U, 1920U, 2176U, 2432U,
	};
	static const size_t expected_peaks[] = {
		109U, 365U, 621U, 877U, 1133U, 1389U, 1645U, 1901U, 2157U, 2413U,
	};
	static const float fixture_shape[] = { 0.25f, 0.75f, 1.0f, 0.75f, 0.25f };
	static const float expected_shape_standardized[] = {
		2.56175921f, 7.93725393f, 10.62500130f, 7.93725393f, 2.56175921f,
	};
	static const float expected_features[] = {
		1000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	};
	static const float expected_standardized_features[] = {
		0.26184893f, -0.72462300f, -0.69026889f, -0.91497965f,
		-1.46363233f, -0.81666524f, -0.62238771f,
	};
	const float expected_baseline_standardized = -0.12598816f;

	reset_guarded_window();
	for (size_t sample_index = 0; sample_index < ECG_PROCESSOR_WINDOW_SIZE;
	     ++sample_index) {
		float sample = 0.0f;

		for (size_t beat = 0; beat < ARRAY_SIZE(fixture_centers); ++beat) {
			size_t shape_start = fixture_centers[beat] - 2U;

			if (sample_index >= shape_start &&
			    sample_index < shape_start + ARRAY_SIZE(fixture_shape)) {
				sample = fixture_shape[sample_index - shape_start];
				break;
			}
		}
		(void)ecg_sample_window_append(&guarded_window.window, sample);
	}

	zassert_ok(ecg_prepare_model_inputs(&guarded_window.window, &workspace,
					    &processing_result));
	zassert_equal(processing_result.r_peak_count, ARRAY_SIZE(expected_peaks));
	for (size_t index = 0; index < ARRAY_SIZE(expected_peaks); ++index) {
		zassert_equal(workspace.r_peak_indices[index], expected_peaks[index],
			      "golden peak mismatch at %u", (unsigned int)index);
	}

	zassert_true(processing_result.rr.features_valid);
	zassert_equal(processing_result.rr.interval_count, ARRAY_SIZE(expected_peaks) - 1U);
	for (size_t index = 0; index < processing_result.rr.interval_count; ++index) {
		zassert_equal(processing_result.rr.intervals_ms[index], 1000.0f,
			      "golden RR mismatch at %u", (unsigned int)index);
	}
	assert_float_array(processing_result.rr.features_unscaled, expected_features,
			   ECG_PROCESSOR_RR_FEATURE_COUNT, FLOAT_TOLERANCE);
	assert_float_array(processing_result.rr.features_standardized,
			   expected_standardized_features, ECG_PROCESSOR_RR_FEATURE_COUNT,
			   FLOAT_TOLERANCE);

	for (size_t sample_index = 0; sample_index < ECG_PROCESSOR_WINDOW_SIZE;
	     ++sample_index) {
		float expected = expected_baseline_standardized;

		for (size_t beat = 0; beat < ARRAY_SIZE(fixture_centers); ++beat) {
			size_t shape_start = fixture_centers[beat] - 2U;

			if (sample_index >= shape_start &&
			    sample_index < shape_start + ARRAY_SIZE(fixture_shape)) {
				expected = expected_shape_standardized[sample_index - shape_start];
				break;
			}
		}
		zassert_within(guarded_window.window.samples[sample_index], expected,
			       FLOAT_TOLERANCE, "golden ECG model input mismatch at %u",
			       (unsigned int)sample_index);
	}
	zassert_equal(guarded_window.before, CANARY_BEFORE);
	zassert_equal(guarded_window.after, CANARY_AFTER);
}

ZTEST(ecg_regression, test_partial_window_cannot_be_prepared)
{
	reset_guarded_window();
	guarded_window.window.samples[0] = 42.0f;
	guarded_window.window.count = ECG_PROCESSOR_WINDOW_SIZE - 1U;

	zassert_equal(ecg_prepare_model_inputs(&guarded_window.window, &workspace,
					       &processing_result),
		      -ENODATA);
	zassert_equal(guarded_window.window.samples[0], 42.0f);
}

ZTEST_SUITE(ecg_decode, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(ecg_window, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(ecg_rr, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(ecg_regression, NULL, NULL, NULL, NULL, NULL);
