#ifndef TINYCARDIA_MAX30003_H_
#define TINYCARDIA_MAX30003_H_

#include <stdbool.h>
#include <stdint.h>

typedef void (*max30003_sample_handler_t)(uint32_t raw_word, uint32_t timestamp_ms,
					 void *user_data);

enum max30003_lead_status {
	MAX30003_LEAD_STATUS_GOOD,
	MAX30003_LEAD_STATUS_NEGATIVE_OFF,
	MAX30003_LEAD_STATUS_POSITIVE_OFF,
	MAX30003_LEAD_STATUS_BOTH_OFF,
	MAX30003_LEAD_STATUS_UNKNOWN,
};

typedef void (*max30003_lead_status_handler_t)(enum max30003_lead_status status,
					       void *user_data);
typedef void (*max30003_drop_handler_t)(uint32_t minimum_dropped, void *user_data);

/* Initializes SPI, INT1, and the ECG acquisition registers. */
int max30003_init(max30003_sample_handler_t sample_handler, void *user_data);

/* Enable or disable the ECG front end and its FIFO interrupt path. */
int max30003_set_monitoring(bool enabled);

/* Current driver-level acquisition state after any transition rollback. */
bool max30003_is_monitoring(void);

/* Register for meaningful DC lead-off transitions detected while monitoring. */
void max30003_set_lead_status_handler(max30003_lead_status_handler_t handler,
				      void *user_data);

/* Register for FIFO discontinuities; the count is a conservative lower bound. */
void max30003_set_drop_handler(max30003_drop_handler_t handler, void *user_data);

int max30003_read_register(uint8_t reg, uint32_t *value);
int max30003_write_register(uint8_t reg, uint32_t value);
int max30003_sanity_check(uint32_t *info);
int max30003_dump_registers(void);

#endif /* TINYCARDIA_MAX30003_H_ */
