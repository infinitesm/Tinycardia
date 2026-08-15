/* SPDX-License-Identifier: MIT */

#include "ecg_processor.h"

#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

K_SEM_DEFINE(handler_entered, 0, 1);
K_SEM_DEFINE(handler_release, 0, 1);
K_SEM_DEFINE(window_completed, 0, 8);

static atomic_t completed_windows;
static atomic_t block_next_handler;
static atomic_t handler_error;
static uint32_t sample_timestamp;

static void prepared_window_handler(const struct ecg_prepared_window *window,
				    void *user_data)
{
	ARG_UNUSED(user_data);

	if (window->sample_count != ECG_PROCESSOR_WINDOW_SIZE) {
		atomic_set(&handler_error, 1);
	}
	if (atomic_cas(&block_next_handler, 1, 0)) {
		k_sem_give(&handler_entered);
		if (k_sem_take(&handler_release, K_SECONDS(2)) < 0) {
			atomic_set(&handler_error, 1);
		}
	}
	atomic_inc(&completed_windows);
	k_sem_give(&window_completed);
}

static void submit_complete_window(void)
{
	for (size_t index = 0U; index < ECG_PROCESSOR_WINDOW_SIZE; ++index) {
		zassert_true(ecg_processor_submit_sample(0U, sample_timestamp++),
			     "sample %u was unexpectedly dropped", (unsigned int)index);
	}
}

ZTEST(ecg_processor, test_double_buffering_and_stopped_window_invalidation)
{
	atomic_set(&block_next_handler, 1);
	zassert_ok(ecg_processor_init(prepared_window_handler, NULL));
	zassert_ok(ecg_processor_set_monitoring(true));

	/* Processing window 1 must not block capture of the complete second window. */
	submit_complete_window();
	zassert_ok(k_sem_take(&handler_entered, K_SECONDS(2)));
	submit_complete_window();
	zassert_false(ecg_processor_submit_sample(0U, sample_timestamp++),
		      "a third window cannot start while both slots are occupied");
	k_sem_give(&handler_release);
	zassert_ok(k_sem_take(&window_completed, K_SECONDS(2)));
	zassert_ok(k_sem_take(&window_completed, K_SECONDS(2)));
	zassert_equal(atomic_get(&completed_windows), 2);
	zassert_equal(atomic_get(&handler_error), 0);

	/* A complete queued window from a stopped session must never be delivered. */
	atomic_set(&block_next_handler, 1);
	submit_complete_window();
	zassert_ok(k_sem_take(&handler_entered, K_SECONDS(2)));
	submit_complete_window();
	zassert_ok(ecg_processor_set_monitoring(false));
	zassert_ok(ecg_processor_set_monitoring(false));
	k_sem_give(&handler_release);
	zassert_ok(k_sem_take(&window_completed, K_SECONDS(2)));
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&completed_windows), 3,
		      "queued stale window reached the application callback");

	zassert_ok(ecg_processor_set_monitoring(true));
	zassert_ok(ecg_processor_set_monitoring(true));
	submit_complete_window();
	zassert_ok(k_sem_take(&window_completed, K_SECONDS(2)));
	zassert_equal(atomic_get(&completed_windows), 4);
	zassert_equal(atomic_get(&handler_error), 0);
}

ZTEST_SUITE(ecg_processor, NULL, NULL, NULL, NULL, NULL);
