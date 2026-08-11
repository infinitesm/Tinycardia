#ifndef TINYCARDIA_MAX30003_H_
#define TINYCARDIA_MAX30003_H_

#include <stdint.h>

typedef void (*max30003_sample_handler_t)(uint32_t raw_word, void *user_data);

/* Initializes SPI, INT1, and the ECG acquisition registers. */
int max30003_init(max30003_sample_handler_t sample_handler, void *user_data);

int max30003_read_register(uint8_t reg, uint32_t *value);
int max30003_write_register(uint8_t reg, uint32_t value);
int max30003_sanity_check(uint8_t *info);
int max30003_dump_registers(void);

#endif /* TINYCARDIA_MAX30003_H_ */
