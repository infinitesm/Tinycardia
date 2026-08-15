#include "model_inference.h"

#include "model_data.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(tinycardia_model, CONFIG_LOG_DEFAULT_LEVEL);

namespace
{

constexpr char kRrInputName[] = "serving_default_rr_features:0";
constexpr char kEcgInputName[] = "serving_default_ecg_signal:0";
constexpr char kOutputName[] = "StatefulPartitionedCall_1:0";
constexpr size_t kExpectedOperatorInstances = 21U;
constexpr size_t kOperatorTypeCount = 8U;
constexpr float kScaleRelativeTolerance = 1.0e-5f;
constexpr int8_t kZeroInputExpectedOutput[] = {-49, 49};
constexpr int kSelfTestOutputTolerance = 1;

alignas(16) uint8_t tensor_arena[CONFIG_TINYCARDIA_MODEL_TENSOR_ARENA_SIZE];
alignas(tflite::MicroInterpreter) uint8_t interpreter_storage[sizeof(tflite::MicroInterpreter)];

const tflite::Model *model;
tflite::MicroInterpreter *interpreter;
TfLiteTensor *rr_input;
TfLiteTensor *ecg_input;
TfLiteTensor *output;
size_t arena_used_bytes;
bool initialized;

tflite::MicroMutableOpResolver<kOperatorTypeCount> resolver;

bool scale_matches(float actual, float expected)
{
	return std::fabs(actual - expected) <= std::fabs(expected) * kScaleRelativeTolerance;
}

template <size_t N>
bool flatbuffer_string_matches(const flatbuffers::String *actual, const char (&expected)[N])
{
	return actual != nullptr && actual->size() == N - 1U &&
	       std::memcmp(actual->Data(), expected, N - 1U) == 0;
}

bool dimensions_match(const TfLiteTensor *tensor, const int *expected, size_t expected_count)
{
	if (tensor == nullptr || tensor->dims == nullptr ||
	    tensor->dims->size != static_cast<int>(expected_count)) {
		return false;
	}

	for (size_t index = 0U; index < expected_count; ++index) {
		if (tensor->dims->data[index] != expected[index]) {
			return false;
		}
	}

	return true;
}

bool tensor_matches(const TfLiteTensor *tensor, const int *dimensions, size_t dimension_count,
		    float scale, int32_t zero_point)
{
	return tensor != nullptr && tensor->type == kTfLiteInt8 &&
	       dimensions_match(tensor, dimensions, dimension_count) &&
	       scale_matches(tensor->params.scale, scale) &&
	       tensor->params.zero_point == zero_point;
}

void log_tensor(const char *label, const TfLiteTensor *tensor)
{
	uint32_t scale_bits = 0U;
	int dimensions[3] = {-1, -1, -1};
	int dimension_count = -1;

	if (tensor == nullptr) {
		LOG_ERR("%s tensor is null", label);
		return;
	}
	std::memcpy(&scale_bits, &tensor->params.scale, sizeof(scale_bits));
	if (tensor->dims != nullptr) {
		dimension_count = tensor->dims->size;
		for (int index = 0; index < dimension_count && index < 3; ++index) {
			dimensions[index] = tensor->dims->data[index];
		}
	}
	LOG_ERR("%s: name=%s type=%d dims=%d [%d,%d,%d] scale_bits=0x%08x zp=%d", label,
		tensor->name == nullptr ? "<null>" : tensor->name, static_cast<int>(tensor->type),
		dimension_count, dimensions[0], dimensions[1], dimensions[2], scale_bits,
		tensor->params.zero_point);
}

int required_operator_index(tflite::BuiltinOperator op)
{
	switch (op) {
	case tflite::BuiltinOperator_EXPAND_DIMS:
		return 0;
	case tflite::BuiltinOperator_CONV_2D:
		return 1;
	case tflite::BuiltinOperator_RESHAPE:
		return 2;
	case tflite::BuiltinOperator_MAX_POOL_2D:
		return 3;
	case tflite::BuiltinOperator_MEAN:
		return 4;
	case tflite::BuiltinOperator_FULLY_CONNECTED:
		return 5;
	case tflite::BuiltinOperator_CONCATENATION:
		return 6;
	case tflite::BuiltinOperator_SOFTMAX:
		return 7;
	default:
		return -1;
	}
}

int validate_operator_set(void)
{
	bool found[kOperatorTypeCount] = {};
	const auto *subgraphs = model->subgraphs();
	const auto *operator_codes = model->operator_codes();

	if (subgraphs == nullptr || subgraphs->size() != 1U || operator_codes == nullptr) {
		LOG_ERR("Model must contain exactly one subgraph");
		return -EINVAL;
	}

	const auto *operators = subgraphs->Get(0)->operators();
	if (operators == nullptr || operators->size() != kExpectedOperatorInstances) {
		LOG_ERR("Model operator count mismatch: %u",
			operators == nullptr ? 0U : static_cast<unsigned int>(operators->size()));
		return -EINVAL;
	}

	for (size_t index = 0U; index < operators->size(); ++index) {
		const auto *op = operators->Get(index);
		if (op == nullptr || op->opcode_index() >= operator_codes->size()) {
			return -EINVAL;
		}

		const auto *code = operator_codes->Get(op->opcode_index());
		int required_index = required_operator_index(code->builtin_code());
		if (required_index < 0) {
			LOG_ERR("Unexpected model operator code %d",
				static_cast<int>(code->builtin_code()));
			return -ENOTSUP;
		}
		found[required_index] = true;
	}

	for (size_t index = 0U; index < kOperatorTypeCount; ++index) {
		if (!found[index]) {
			LOG_ERR("Required model operator type %u is absent",
				static_cast<unsigned int>(index));
			return -EINVAL;
		}
	}

	return 0;
}

int register_operators(void)
{
	if (resolver.AddExpandDims() != kTfLiteOk || resolver.AddConv2D() != kTfLiteOk ||
	    resolver.AddReshape() != kTfLiteOk || resolver.AddMaxPool2D() != kTfLiteOk ||
	    resolver.AddMean() != kTfLiteOk || resolver.AddFullyConnected() != kTfLiteOk ||
	    resolver.AddConcatenation() != kTfLiteOk || resolver.AddSoftmax() != kTfLiteOk) {
		LOG_ERR("Unable to register required TFLM operators");
		return -ENOMEM;
	}

	return 0;
}

int validate_tensor_contract(void)
{
	static const int rr_dimensions[] = {1, 7};
	static const int ecg_dimensions[] = {1, 2560, 1};
	static const int output_dimensions[] = {1, 2};
	const auto *subgraph = model->subgraphs()->Get(0);
	const auto *model_inputs = subgraph->inputs();
	const auto *model_outputs = subgraph->outputs();
	const auto *model_tensors = subgraph->tensors();
	int rr_input_index = -1;
	int ecg_input_index = -1;

	if (interpreter->inputs_size() != 2U || interpreter->outputs_size() != 1U) {
		LOG_ERR("Model input/output count mismatch: %u/%u",
			static_cast<unsigned int>(interpreter->inputs_size()),
			static_cast<unsigned int>(interpreter->outputs_size()));
		return -EINVAL;
	}

	/* TFLM omits names from TfLiteTensor; resolve them from the FlatBuffer. */
	for (size_t index = 0U; index < model_inputs->size(); ++index) {
		const auto *tensor = model_tensors->Get(model_inputs->Get(index));

		if (flatbuffer_string_matches(tensor->name(), kRrInputName)) {
			rr_input_index = static_cast<int>(index);
		} else if (flatbuffer_string_matches(tensor->name(), kEcgInputName)) {
			ecg_input_index = static_cast<int>(index);
		}
	}
	const auto *model_output = model_tensors->Get(model_outputs->Get(0));
	if (rr_input_index != 0 || ecg_input_index != 1 ||
	    !flatbuffer_string_matches(model_output->name(), kOutputName)) {
		LOG_ERR("Embedded model tensor name/order mismatch: RR=%d ECG=%d OUT=%s",
			rr_input_index, ecg_input_index,
			flatbuffer_string_matches(model_output->name(), kOutputName) ? "match"
										     : "mismatch");
		return -EINVAL;
	}

	rr_input = interpreter->input(static_cast<size_t>(rr_input_index));
	ecg_input = interpreter->input(static_cast<size_t>(ecg_input_index));
	output = interpreter->output(0);

	if (!tensor_matches(rr_input, rr_dimensions, ARRAY_SIZE(rr_dimensions),
			    TINYCARDIA_MODEL_RR_SCALE, TINYCARDIA_MODEL_RR_ZERO_POINT) ||
	    !tensor_matches(ecg_input, ecg_dimensions, ARRAY_SIZE(ecg_dimensions),
			    TINYCARDIA_MODEL_ECG_SCALE, TINYCARDIA_MODEL_ECG_ZERO_POINT) ||
	    !tensor_matches(output, output_dimensions, ARRAY_SIZE(output_dimensions),
			    TINYCARDIA_MODEL_OUTPUT_SCALE, TINYCARDIA_MODEL_OUTPUT_ZERO_POINT)) {
		LOG_ERR("Embedded model tensor contract mismatch");
		log_tensor("RR input", rr_input);
		log_tensor("ECG input", ecg_input);
		log_tensor("output 0", output);
		rr_input = nullptr;
		ecg_input = nullptr;
		output = nullptr;
		return -EINVAL;
	}

	return 0;
}

int run_startup_self_test(void)
{
	for (size_t index = 0U; index < TINYCARDIA_MODEL_RR_COUNT; ++index) {
		rr_input->data.int8[index] = TINYCARDIA_MODEL_RR_ZERO_POINT;
	}
	for (size_t index = 0U; index < TINYCARDIA_MODEL_ECG_COUNT; ++index) {
		ecg_input->data.int8[index] = TINYCARDIA_MODEL_ECG_ZERO_POINT;
	}
	if (interpreter->Invoke() != kTfLiteOk) {
		LOG_ERR("TFLM startup self-test Invoke() failed");
		return -EIO;
	}

	for (size_t index = 0U; index < ARRAY_SIZE(kZeroInputExpectedOutput); ++index) {
		int difference = static_cast<int>(output->data.int8[index]) -
				 static_cast<int>(kZeroInputExpectedOutput[index]);

		if (difference < -kSelfTestOutputTolerance ||
		    difference > kSelfTestOutputTolerance) {
			LOG_ERR("TFLM startup self-test mismatch at %u: expected %d, got %d",
				static_cast<unsigned int>(index),
				static_cast<int>(kZeroInputExpectedOutput[index]),
				static_cast<int>(output->data.int8[index]));
			return -EIO;
		}
	}

	return 0;
}

uint16_t probability_to_confidence(float probability)
{
	long confidence;

	if (probability < 0.0f) {
		probability = 0.0f;
	} else if (probability > 1.0f) {
		probability = 1.0f;
	}
	confidence = std::lround(probability * 10000.0f);

	return static_cast<uint16_t>(confidence);
}

} /* namespace */

extern "C" int tinycardia_model_init(void)
{
	int err;

	if (initialized) {
		return -EALREADY;
	}
	if (tinycardia_model_data_size != 91816U) {
		LOG_ERR("Embedded model size mismatch: %u",
			static_cast<unsigned int>(tinycardia_model_data_size));
		return -EINVAL;
	}

	model = tflite::GetModel(tinycardia_model_data);
	if (model == nullptr || model->version() != TFLITE_SCHEMA_VERSION) {
		LOG_ERR("Unsupported model schema: model %d, runtime %d",
			model == nullptr ? -1 : model->version(), TFLITE_SCHEMA_VERSION);
		return -ENOTSUP;
	}

	err = validate_operator_set();
	if (err < 0) {
		return err;
	}
	err = register_operators();
	if (err < 0) {
		return err;
	}

	interpreter = new (interpreter_storage)
		tflite::MicroInterpreter(model, resolver, tensor_arena, sizeof(tensor_arena));
	if (interpreter->AllocateTensors() != kTfLiteOk) {
		LOG_ERR("TFLM tensor allocation failed with %u-byte arena",
			static_cast<unsigned int>(sizeof(tensor_arena)));
		interpreter = nullptr;
		return -ENOMEM;
	}

	err = validate_tensor_contract();
	if (err < 0) {
		return err;
	}
	err = run_startup_self_test();
	if (err < 0) {
		return err;
	}

	arena_used_bytes = interpreter->arena_used_bytes();
	initialized = true;
	LOG_INF("AFib model ready: %u-byte artifact, arena %u/%u bytes, CMSIS-NN %s",
		static_cast<unsigned int>(tinycardia_model_data_size),
		static_cast<unsigned int>(arena_used_bytes),
		static_cast<unsigned int>(sizeof(tensor_arena)),
		IS_ENABLED(CONFIG_TENSORFLOW_LITE_MICRO_CMSIS_NN_KERNELS) ? "enabled" : "disabled");

	return 0;
}

extern "C" int tinycardia_model_infer(const float *ecg, size_t ecg_count, const float *rr_features,
				      size_t rr_count, struct tinycardia_model_result *result)
{
	uint64_t start_cycles;
	uint64_t elapsed_cycles;
	size_t selected_index;
	float selected_probability;

	if (!initialized || interpreter == nullptr) {
		return -EACCES;
	}
	if (ecg == nullptr || rr_features == nullptr || result == nullptr) {
		return -EINVAL;
	}
	if (ecg_count != TINYCARDIA_MODEL_ECG_COUNT || rr_count != TINYCARDIA_MODEL_RR_COUNT) {
		return -EMSGSIZE;
	}

	for (size_t index = 0U; index < rr_count; ++index) {
		rr_input->data.int8[index] =
			tinycardia_model_quantize(rr_features[index], TINYCARDIA_MODEL_RR_SCALE,
						  TINYCARDIA_MODEL_RR_ZERO_POINT);
	}
	for (size_t index = 0U; index < ecg_count; ++index) {
		ecg_input->data.int8[index] = tinycardia_model_quantize(
			ecg[index], TINYCARDIA_MODEL_ECG_SCALE, TINYCARDIA_MODEL_ECG_ZERO_POINT);
	}

	start_cycles = k_cycle_get_64();
	if (interpreter->Invoke() != kTfLiteOk) {
		LOG_ERR("TFLM Invoke() failed");
		return -EIO;
	}
	elapsed_cycles = k_cycle_get_64() - start_cycles;

	result->quantized_probabilities[0] = output->data.int8[0];
	result->quantized_probabilities[1] = output->data.int8[1];
	selected_index = output->data.int8[1] > output->data.int8[0] ? 1U : 0U;
	selected_probability = tinycardia_model_dequantize(output->data.int8[selected_index],
							   TINYCARDIA_MODEL_OUTPUT_SCALE,
							   TINYCARDIA_MODEL_OUTPUT_ZERO_POINT);

	/* Notebook LabelEncoder classes_: [atrial fibrillation, sinus normal]. */
	result->classification = selected_index == 0U ? TINYCARDIA_CLASSIFICATION_AFIB
						      : TINYCARDIA_CLASSIFICATION_NORMAL;
	result->confidence = probability_to_confidence(selected_probability);
	result->invoke_time_us = static_cast<uint32_t>(k_cyc_to_us_floor64(elapsed_cycles));

	return 0;
}

extern "C" size_t tinycardia_model_arena_used_bytes(void)
{
	return arena_used_bytes;
}
