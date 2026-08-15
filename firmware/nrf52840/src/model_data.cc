#include "model_data.h"

#include <cstddef>

alignas(16) const unsigned char tinycardia_model_data[] = {
#include "afib_detector_int8.inc"
};

const size_t tinycardia_model_data_size = sizeof(tinycardia_model_data);

static_assert(sizeof(tinycardia_model_data) == 91816U,
	      "embedded Tinycardia model size differs from canonical artifact");
