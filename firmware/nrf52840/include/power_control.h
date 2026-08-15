#ifndef TINYCARDIA_POWER_CONTROL_H_
#define TINYCARDIA_POWER_CONTROL_H_

/*
 * Blocks until the power button has been held continuously for three seconds.
 * A released or short-pressed button returns the nRF52840 to System OFF.
 */
int power_control_wait_for_on(void);

/*
 * Starts the runtime button monitor. After the initial power-on button is
 * released, another continuous three-second hold enters System OFF.
 */
int power_control_start_off_monitor(void);

/* Configures the power button as a wake source and enters System OFF. */
void power_control_enter_ultra_low_power(void);

#endif /* TINYCARDIA_POWER_CONTROL_H_ */
