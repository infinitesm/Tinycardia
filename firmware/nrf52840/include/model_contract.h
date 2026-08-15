#ifndef TINYCARDIA_MODEL_CONTRACT_H_
#define TINYCARDIA_MODEL_CONTRACT_H_

#include <math.h>
#include <stdint.h>

#define TINYCARDIA_MODEL_ECG_COUNT   2560U
#define TINYCARDIA_MODEL_RR_COUNT    7U
#define TINYCARDIA_MODEL_CLASS_COUNT 2U

#define TINYCARDIA_MODEL_RR_SCALE          0.014579945243895054f
#define TINYCARDIA_MODEL_RR_ZERO_POINT     (-27)
#define TINYCARDIA_MODEL_ECG_SCALE         0.04720002040266991f
#define TINYCARDIA_MODEL_ECG_ZERO_POINT    (-15)
#define TINYCARDIA_MODEL_OUTPUT_SCALE      0.00390625f
#define TINYCARDIA_MODEL_OUTPUT_ZERO_POINT (-128)

static inline int8_t tinycardia_model_quantize(float value, float scale, int32_t zero_point)
{
	long quantized = lroundf(value / scale) + zero_point;

	if (quantized < INT8_MIN) {
		quantized = INT8_MIN;
	} else if (quantized > INT8_MAX) {
		quantized = INT8_MAX;
	}

	return (int8_t)quantized;
}

static inline float tinycardia_model_dequantize(int8_t value, float scale, int32_t zero_point)
{
	return ((float)value - (float)zero_point) * scale;
}

#endif /* TINYCARDIA_MODEL_CONTRACT_H_ */
