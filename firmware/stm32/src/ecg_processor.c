#include "ecg_processor.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "afib_detector.h"

#define WINDOW_SIZE   (AI_AFIB_DETECTOR_IN_1_SIZE_BYTES)  // 2560
#define RR_FEAT_SIZE  (AI_AFIB_DETECTOR_IN_2_SIZE_BYTES)  //    7
#define FS_HZ         256

// quantization params:
#define IN0_SCALE     (0.04720002f)
#define IN0_ZP        (-15)
#define IN1_SCALE     (0.015179742f)
#define IN1_ZP        (-32)

// integration & peak detection windows:
#define MA_WIN        ((int)(0.15f * FS_HZ))  // ~38 samples (150ms)
#define MIN_DIST      ((int)(0.78f * FS_HZ))  // ~200 samples (780ms)

// RR feature standardization constants
const float RR_MEANS[7] = 
{
    960.825116f,
    99.9119008f,
    146.620803f,
    0.311937086f,
    0.477063449f,
    100.942917f,
    74.6044621f
};

const float RR_STDS[7] = 
{
    149.60872127f,
    137.881217f,
    212.41114191f,
    0.34092243f,
    0.32594487f,
    123.60378762f,
    119.86814799f
};

// sliding window
static float    ecg_buffer[WINDOW_SIZE];
static uint16_t buf_idx     = 0;
static bool     window_flag = false;

// UART handle for debug & results
static UART_HandleTypeDef *huart;

void ECG_Processor_Init(UART_HandleTypeDef *uart)
{
  huart       = uart;
  buf_idx     = 0;
  window_flag = false;
}

// process a reading from the ECG front end
void ECG_Processor_ProcessSample(uint32_t raw_word)
{
  // extract 18-bit signed sample
  uint32_t u18 = (raw_word >> 6) & 0x3FFFF;
  int32_t  s18 = (u18 & (1<<17)) ? (int32_t)(u18 | ~((1<<18)-1)) : (int32_t)u18;

  // convert to mV
  float gain = 32.5f;
  float resolution = 131072.0f;

  float mV = s18 * (gain / resolution);

  // store in window
  ecg_buffer[buf_idx++] = mV;

  // set the flag so we know samples are ready to process
  if (buf_idx >= WINDOW_SIZE) 
  {
    buf_idx     = 0;
    window_flag = true;
  }

  // send ecg voltage to laptop for live waveform display
  {
    char dbuf[48];
    uint32_t t = HAL_GetTick();
    int len = snprintf(dbuf, sizeof(dbuf),
                       "[%lu ms] ECG= %+0.3f mV\r\n",
                       (unsigned long)t, mV);
    HAL_UART_Transmit(huart, (uint8_t*)dbuf, len, HAL_MAX_DELAY);
  }
}

// check if the window is ready to process, if it is, reset the flag.
bool ECG_Processor_WindowReady(void)
{
  if (window_flag) {
    window_flag = false;
    return true;
  }
  return false;
}

void ECG_Processor_PrepareInput(ai_i8 *data_ins[])
{
  // standardize the ECG window
  float mean_w = 0, std_w = 0;

  for (int i = 0; i < WINDOW_SIZE; i++) 
  {
    mean_w += ecg_buffer[i];
  }

  mean_w /= WINDOW_SIZE;

  for (int i = 0; i < WINDOW_SIZE; i++)
  {
    float d = ecg_buffer[i] - mean_w;
    std_w += d*d;
  }

  std_w = sqrtf(std_w / WINDOW_SIZE);

  if (std_w < 1e-6f) std_w = 1e-6f;

  for (int i = 0; i < WINDOW_SIZE; i++) 
  {
    ecg_buffer[i] = (ecg_buffer[i] - mean_w) / std_w;
  }

  // QRS complex detection (simplified Pan-Tompkind)
  static float diff_buf[WINDOW_SIZE];
  static float sq_buf[WINDOW_SIZE];
  static float int_buf[WINDOW_SIZE];
  int peaks[64];
  int n_peaks = 0;

  // compute derivative & sqr derivative
  diff_buf[0] = 0;
  for (int i = 1; i < WINDOW_SIZE; i++)
  {
      float d = ecg_buffer[i] - ecg_buffer[i - 1];
      diff_buf[i] = d;
      sq_buf[i] = d * d;
  }

  // moving average integration
  for (int i = 0; i < WINDOW_SIZE; i++) 
  {
      int start = i - MA_WIN / 2;
      int end = i + MA_WIN / 2;
      if (start < 0) start = 0;
      if (end >= WINDOW_SIZE) end = WINDOW_SIZE - 1;

      float sum = 0;
      for (int j = start; j <= end; j++) 
      {
          sum += sq_buf[j];
      }
      int_buf[i] = sum / (float)(end - start + 1);
  }

  // calculate threshold
  float m = 0, v = 0;

  for (int i = 0; i < WINDOW_SIZE; i++) 
  {
    m += int_buf[i];
  }

  m /= WINDOW_SIZE;

  for (int i = 0; i < WINDOW_SIZE; i++) 
  {
      float d = int_buf[i] - m;
      v += d * d;
  }

  float sd = sqrtf(v / WINDOW_SIZE);
  float thr = m + 0.7f * sd;

  // peak detection
  int last_peak_idx = -MIN_DIST;
  int in_peak = 0;
  float peak_val = 0.0f;
  int peak_idx = -1;

  for (int i = 0; i < WINDOW_SIZE; i++) 
  {
      if (int_buf[i] > thr) 
      {
          if (!in_peak) 
          {
              in_peak = 1;
              peak_val = int_buf[i];
              peak_idx = i;
          } 
          else 
          {
              if (int_buf[i] > peak_val) 
              {
                  peak_val = int_buf[i];
                  peak_idx = i;
              }
          }
      } 
      else 
      {
          if (in_peak) 
          {
              if ((peak_idx - last_peak_idx) > MIN_DIST) 
              {
                  peaks[n_peaks++] = peak_idx;
                  last_peak_idx = peak_idx;
                  if (n_peaks >= 64) break;
              }
              in_peak = 0;
              peak_val = 0.0f;
              peak_idx = -1;
          }
      }
  }

  // handle if the last peak extends to end of buffer
  if (in_peak && (peak_idx - last_peak_idx) > MIN_DIST) 
  {
      peaks[n_peaks++] = peak_idx;
  }

  // compute RR features 
  float features[RR_FEAT_SIZE] = {0};
  int   n_rr = n_peaks - 1;
  if (n_rr >= 2) 
  {
    float rr[64], sum_rr = 0;

    for (int k = 0; k < n_rr; k++) 
    {
      rr[k] = (peaks[k+1] - peaks[k]) * 1000.0f / FS_HZ;
      sum_rr += rr[k];
    }

    float mean_rr = sum_rr / n_rr;
    float var_rr = 0;

    for (int k = 0; k < n_rr; k++) 
    {
      float d = rr[k] - mean_rr;
      var_rr += d*d;
    }

    var_rr /= (n_rr - 1);

    float sdnn = sqrtf(var_rr);

    int cnt50 = 0, cnt20 = 0;
    float sum_d2 = 0;

    for (int k = 0; k < n_rr-1; k++) 
    {
      float dd = rr[k+1] - rr[k];
      sum_d2 += dd*dd;
      if (fabsf(dd) > 50.0f) cnt50++;
      if (fabsf(dd) > 20.0f) cnt20++;
    }

    float rmssd = sqrtf(sum_d2 / (n_rr - 1));
    float pnn50 = (float)cnt50 / (n_rr - 1);
    float pnn20 = (float)cnt20 / (n_rr - 1);

    float var_d = sum_d2 / (n_rr - 1);
    float sd1   = sqrtf(var_d/2.0f);
    float sd2s  = 2*var_rr - var_d/2.0f;
    float sd2   = (sd2s>0 ? sqrtf(sd2s) : 0.0f);

    features[0] = mean_rr;
    features[1] = sdnn;
    features[2] = rmssd;
    features[3] = pnn50;
    features[4] = pnn20;
    features[5] = sd1;
    features[6] = sd2;
  }

  // normalize rr features
  for (int j = 0; j < RR_FEAT_SIZE; j++) 
  {
    features[j] = (features[j] - RR_MEANS[j]) / RR_STDS[j];
  }

  // quantize and push to model data buffer
  for (int i = 0; i < WINDOW_SIZE; i++) 
  {
    int32_t q = (int32_t)roundf(ecg_buffer[i] / IN0_SCALE) + IN0_ZP;
    if (q > 127) q = 127;
    else if (q < -128) q = -128;
    data_ins[0][i] = (ai_i8)q;
  }

  for (int j = 0; j < RR_FEAT_SIZE; j++) 
  {
    int32_t q = (int32_t)roundf(features[j] / IN1_SCALE) + IN1_ZP;
    if (q > 127) q = 127;
    else if (q < -128) q = -128;
    data_ins[1][j] = (ai_i8)q;
  }
}

void ECG_Processor_HandleInferenceResult(ai_i8 *data_outs[])
{
  // quantization params from validation report:
  const float OUT_SCALE = 0.00390625f;
  const int   OUT_ZP    = -128;

  // raw int8 outputs
  int8_t q0 = data_outs[0][0];
  int8_t q1 = data_outs[0][1];

  // convert back to floats
  float p_sinus = OUT_SCALE * ((int)q0 - OUT_ZP);
  float p_afib  = OUT_SCALE * ((int)q1 - OUT_ZP);

  // renormalize in case of rounding drift
  // (not sure this is right, suggested by LLM)
  float s = p_sinus + p_afib;
  if (s > 1e-6f) 
  {
    p_sinus /= s;
    p_afib  /= s;
  }

  // send data to laptop for live display
  char buf[80];
  int len = snprintf(buf, sizeof(buf),
    "Sinus = %.3f, AFib = %.3f  (sum=%.3f)\r\n",
    p_sinus, p_afib, p_sinus + p_afib);
  HAL_UART_Transmit(huart, (uint8_t*)buf, len, HAL_MAX_DELAY);
}
