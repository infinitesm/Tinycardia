/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "max30003.h"
#include "ecg_processor.h"

/* Cube.AI headers */
#include "afib_detector.h"
#include "afib_detector_data.h"
#include "ai_datatypes_defines.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

// i/o & buffers
AI_ALIGNED(4)   ai_i8  data_in_1[AI_AFIB_DETECTOR_IN_1_SIZE_BYTES];
AI_ALIGNED(4)   ai_i8  data_in_2[AI_AFIB_DETECTOR_IN_2_SIZE_BYTES];
                ai_i8* data_ins[AI_AFIB_DETECTOR_IN_NUM] = {
                  data_in_1,
                  data_in_2
                };

AI_ALIGNED(4)   ai_i8  data_out_1[AI_AFIB_DETECTOR_OUT_1_SIZE_BYTES];
                ai_i8* data_outs[AI_AFIB_DETECTOR_OUT_NUM] = {
                  data_out_1
                };

// array of activation addresses for the runtime
AI_ALIGNED(32)
static ai_u8 activations[AI_AFIB_DETECTOR_DATA_ACTIVATION_1_SIZE];
static const ai_handle act_addr[] = {
  (ai_handle)activations
};

static ai_handle afib_detector = AI_HANDLE_NULL;
static ai_buffer *ai_input, *ai_output;
/* USER CODE END PV */

void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  // mcu config
  HAL_Init();
  SystemClock_Config();

  // init all peripheral
  MX_GPIO_Init();
  MX_SPI1_Init();
  MX_USART2_UART_Init();

  /* USER CODE BEGIN 2 */
  // device init
  MAX30003_Init(&hspi1, &huart2);
  ECG_Processor_Init(&huart2);
  MAX30003_InitRegisters();

  // network & data pipeline init
  {
    ai_error err = ai_afib_detector_create_and_init(
                     &afib_detector,
                     act_addr,
                     NULL
                   );
    if (err.type != AI_ERROR_NONE) {
      Error_Handler();
    }
    ai_input  = ai_afib_detector_inputs_get (afib_detector, NULL);
    ai_output = ai_afib_detector_outputs_get(afib_detector, NULL);
    /* Bind our I/O buffers */
    ai_input[0].data  = data_ins[0];
    ai_input[1].data  = data_ins[1];
    ai_output[0].data = data_outs[0];
  }

  {
    char buf[64];
    int  n = snprintf(buf, sizeof(buf),
                      "MAX30003 started ECG acquisition.\r\n");
    HAL_UART_Transmit(&huart2, (uint8_t*)buf, n, HAL_MAX_DELAY);
  }
  /* USER CODE END 2 */

  /* USER CODE BEGIN WHILE */
  while (1)
  {
    // if we have full window of data to handle
    if (ECG_Processor_WindowReady()) {
      // extract features, preprocess, send to data buffers
      ECG_Processor_PrepareInput(data_ins);

      // inference
      if ( ai_afib_detector_run(afib_detector, ai_input, ai_output) != 1 ) {
        Error_Handler();
      }

      // send result to laptop
      ECG_Processor_HandleInferenceResult(data_outs);
    }

    // save cpu resource (WFI = wait for interrupt)
    __WFI();
  }
  /* USER CODE END WHILE */

  /* USER CODE BEGIN 3 */
  /* USER CODE END 3 */
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM            = 8;
  RCC_OscInitStruct.PLL.PLLN            = 180;
  RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ            = 2;
  RCC_OscInitStruct.PLL.PLLR            = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  if (HAL_PWREx_EnableOverDrive() != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK   |
                                     RCC_CLOCKTYPE_SYSCLK |
                                     RCC_CLOCKTYPE_PCLK1  |
                                     RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
    Error_Handler();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) { }
}

#ifdef  USE_FULL_ASSERT

void assert_failed(uint8_t *file, uint32_t line)
{ }
#endif /* USE_FULL_ASSERT */
