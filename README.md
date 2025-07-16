# Tinycardia

Lightweight ML cardiac emergency detection designed for embedded systems.

Tinycardia detects atrial fibrillation (AFib) from ECG signals using machine learning models small enough to run on STM32 microcontrllers. It combines deep learning on raw ECG waveforms with extracted RR interval features for robust arrhythmia detection. The project aims to bring real-time, low-power cardiac monitoring to wearable and portable devices.

## Features
- Classifies sinus rhythms vs. atrial fibrillation
- Based on the PTB-XL ECG dataset
- Data augmentation to handle class imbalance
- Thresholding policy to reduce impact of false classifications
- NN branching using 1D convolutional neural network + dense layer
- Model quantization for deployment on microcontroller
- Designed specifically for STM32 microcontrollers

## Design
- Uses a single-lead ECG front end (MAX30003) to obtain raw ECG waveform
- MCU uses SPI communication protocol to communicate with the ECG front end
- Samples ECG signal at 256Hz for balance between data quality & processing overhead
- 10 second ECG windows queue for processing
- Uses Pan-Tompkins algorithm to detect QRS complexes
- After QRS complex detection, extract RR interval features for temporal analysis
- Model infers based on morphological data from 1DCNN branch, and temporal data from RR feature branch.
- Model is quantized for fast integer math usage on STM32 to reduce overhead.
- Designed for low power consumption suitable for wearable device.
- Send ECG voltage data and inference result over UART for real-time monitoring using Python GUI

## Performance
- AFib recall: 95.3%
- AFib precision: 53.2%
- Overall accuracy: 93.7%

**All visualizations can be seen in /model/Tinycardia.ipynb**

In prototype stage, Tinycardia delivers performance metrics that can compare to real world devices such as Apple Watch. However, there are still major improvements to be made in the near future. Currently, Apple Watch is much more precise with AFib classifications, making it more reliable than Tinycardia.

## Dataset
This project uses the PTB-XL dataset:
https://physionet.org/content/ptb-xl/

## Future Improvements
- Improve precision to reduce false positives & reliance on thresholding policy
- Extend to multi-class arrhythmia detection
- Optimize models further
- Move from prototype format to wearable enclosure

## License
MIT License.

## Acknowledgments
Thank you to Protocentral for providing a great example code which helped me get started on MAX30003 development.
https://github.com/Protocentral/protocentral_max30003
