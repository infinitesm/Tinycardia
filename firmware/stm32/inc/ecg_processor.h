#ifndef ECG_PROCESSOR_H
#define ECG_PROCESSOR_H

#include "stm32f4xx_hal.h"
#include "ai_datatypes_defines.h"   // for ai_i8

/// Must be called once after MX_USARTx_UART_Init()
void ECG_Processor_Init(UART_HandleTypeDef *huart);

/// Feed every raw 32-bit word from MAX30003 into the window buffer
void ECG_Processor_ProcessSample(uint32_t raw_word);

/// Returns true exactly once when you’ve filled WINDOW_SIZE samples
bool ECG_Processor_WindowReady(void);

/// Called just before ai_run(): pack your floats → int8 inputs
void ECG_Processor_PrepareInput(ai_i8 *data_ins[]);

/// Called just after ai_run(): unpack int8 outputs & UART-print them
void ECG_Processor_HandleInferenceResult(ai_i8 *data_outs[]);

#endif // ECG_PROCESSOR_H
