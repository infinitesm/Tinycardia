#include "power_control.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>

#define POWER_BUTTON_NODE   DT_ALIAS(power_button)
#define POWER_HOLD_TIME_MS  3000
#define POWER_POLL_TIME     K_MSEC(20)

static const struct gpio_dt_spec power_button =
	GPIO_DT_SPEC_GET(POWER_BUTTON_NODE, gpios);

void power_control_enter_ultra_low_power(void)
{
	/* A level interrupt configures the nRF GPIO SENSE field used in System OFF. */
	(void)gpio_pin_interrupt_configure_dt(&power_button,
					      GPIO_INT_LEVEL_ACTIVE);
	sys_poweroff();
}

int power_control_wait_for_on(void)
{
	int pressed;
	int64_t pressed_at;
	int err;

	// is GPIO controller ready?
	if (!gpio_is_ready_dt(&power_button)) {
		return -ENODEV; // no such device
	}

	// configure P0.06 as input
	err = gpio_pin_configure_dt(&power_button, GPIO_INPUT);
	if (err < 0) {
		return err;
	}

	pressed = gpio_pin_get_dt(&power_button);
	if (pressed < 0) { // invalid state, error occurred
		return pressed;
	}
	if (pressed == 0) { // not pressed (yes it's configured active low, zephyr gives logical state)
		power_control_enter_ultra_low_power();
	}

	pressed_at = k_uptime_get();

	while (k_uptime_get() - pressed_at < POWER_HOLD_TIME_MS) { // while button held less than 3000 ms
		k_sleep(POWER_POLL_TIME); // allow up to 50 Hz polling (1000 ms / 20 ms = 50 Hz)

		// handle loop reset conditions
		pressed = gpio_pin_get_dt(&power_button);
		if (pressed < 0) {
			return pressed;
		}
		if (pressed == 0) {
			power_control_enter_ultra_low_power();
		}
	}

	/* The wake sense is not needed while the application is running. */
	return gpio_pin_interrupt_configure_dt(&power_button, GPIO_INT_DISABLE);
}
