#ifndef TINYCARDIA_INFERENCE_POLICY_H_
#define TINYCARDIA_INFERENCE_POLICY_H_

#include "ble_protocol.h"
#include <stdbool.h>
#include <stdint.h>

static inline bool
tinycardia_inference_window_is_eligible(bool rr_features_valid,
					enum tinycardia_signal_quality current_quality,
					uint64_t window_start_ms, uint64_t quality_good_since_ms)
{
	return rr_features_valid && current_quality == TINYCARDIA_SIGNAL_QUALITY_GOOD &&
	       window_start_ms >= quality_good_since_ms;
}

#endif /* TINYCARDIA_INFERENCE_POLICY_H_ */
