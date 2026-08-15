#include "power_control.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#define POWER_BUTTON_NODE   DT_ALIAS(power_button)
#define STATUS_LED_NODE     DT_ALIAS(led0)
#define POWER_HOLD_TIME_MS  3000
#define POWER_POLL_TIME     K_MSEC(20)
#define POWER_DEBUG_HELD_REPORT_MS 250
#define POWER_DEBUG_IDLE_REPORT_MS 1000
#define POWER_MONITOR_STACK_SIZE 1024
#define POWER_MONITOR_PRIORITY   7

LOG_MODULE_REGISTER(power_control, CONFIG_LOG_DEFAULT_LEVEL);

static const struct gpio_dt_spec power_button =
	GPIO_DT_SPEC_GET(POWER_BUTTON_NODE, gpios);
static const struct gpio_dt_spec status_led =
	GPIO_DT_SPEC_GET(STATUS_LED_NODE, gpios);
static struct k_thread power_monitor_thread;
static bool power_monitor_started;
K_THREAD_STACK_DEFINE(power_monitor_stack, POWER_MONITOR_STACK_SIZE);

static int status_led_configure(bool on)
{
	if (!gpio_is_ready_dt(&status_led)) {
		LOG_ERR("Status LED GPIO controller is not ready");
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&status_led,
				     on ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
}

void power_control_enter_ultra_low_power(void)
{
	int err = status_led_configure(false);

	if (err < 0) {
		LOG_ERR("Failed to turn status LED off before System OFF: %d", err);
	}

	/* A level interrupt configures the nRF GPIO SENSE field used in System OFF. */
	(void)gpio_pin_interrupt_configure_dt(&power_button,
					      GPIO_INT_LEVEL_ACTIVE);
	sys_poweroff();
}

#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)

static int power_control_wait_for_on_debug(void)
{
	int previous_logical = -1;
	int64_t pressed_at = 0;
	int64_t last_report_at = 0;

	LOG_WRN("POWER BUTTON DEBUG ACTIVE: logging power-on hold");
	LOG_INF("Button GPIO controller=%s pin=%u flags=0x%x (active-low)",
		power_button.port->name, (unsigned int)power_button.pin,
		(unsigned int)power_button.dt_flags);

	while (true) {
		int logical = gpio_pin_get_dt(&power_button);
		int raw = gpio_pin_get_raw(power_button.port, power_button.pin);
		int64_t now = k_uptime_get();
		int64_t held_ms = pressed_at == 0 ? 0 : now - pressed_at;

		if (logical < 0) {
			LOG_ERR("power_control_wait_for_on returning %d: logical GPIO read failed",
				logical);
			return logical;
		}
		if (raw < 0) {
			LOG_ERR("power_control_wait_for_on returning %d: raw GPIO read failed", raw);
			return raw;
		}

		if (logical != previous_logical) {
			if (logical != 0) {
				pressed_at = now;
				held_ms = 0;
				LOG_INF("BUTTON=PRESSED raw=%d logical=%d held=0/%d ms", raw,
					logical, POWER_HOLD_TIME_MS);
			} else {
				LOG_INF("BUTTON=RELEASED raw=%d logical=%d previous_hold=%lld ms; entering System OFF",
					raw, logical, (long long)held_ms);
				pressed_at = 0;
				held_ms = 0;
			}
			previous_logical = logical;
			last_report_at = now;
		}

		if (logical != 0) {
			held_ms = now - pressed_at;
			if (held_ms >= POWER_HOLD_TIME_MS) {
				LOG_INF("POWER_ON=ACCEPTED held=%lld ms",
					(long long)held_ms);
				return 0;
			}
			if (now - last_report_at >= POWER_DEBUG_HELD_REPORT_MS) {
				LOG_INF("BUTTON=PRESSED raw=%d logical=%d held=%lld/%d ms",
					raw, logical, (long long)held_ms, POWER_HOLD_TIME_MS);
				last_report_at = now;
			}
		} else {
			power_control_enter_ultra_low_power();
		}

		k_sleep(POWER_POLL_TIME);
	}
}

#endif /* CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG */

int power_control_wait_for_on(void)
{
#if !defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
	int pressed;
	int64_t pressed_at;
#endif
	int err;

	/* A wake-up is not an accepted power-on until the full hold completes. */
	err = status_led_configure(false);
	if (err < 0) {
		return err;
	}

	// is GPIO controller ready?
	if (!gpio_is_ready_dt(&power_button)) {
		LOG_ERR("power_control_wait_for_on returning %d: GPIO controller is not ready",
			-ENODEV);
		return -ENODEV; // no such device
	}

	// configure P0.06 as input
	err = gpio_pin_configure_dt(&power_button, GPIO_INPUT);
	if (err < 0) {
		LOG_ERR("power_control_wait_for_on returning %d: GPIO configuration failed",
			err);
		return err;
	}

#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
	err = power_control_wait_for_on_debug();
	if (err < 0) {
		return err;
	}
#else
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
#endif

	/* The wake sense is not needed while the application is running. */
	err = gpio_pin_interrupt_configure_dt(&power_button, GPIO_INT_DISABLE);
	if (err < 0) {
		return err;
	}

	/* The system is ON only after the continuous three-second hold is accepted. */
	err = gpio_pin_set_dt(&status_led, 1);
	if (err < 0) {
		LOG_ERR("Failed to turn status LED on: %d", err);
	}

	return err;
}

static void power_control_off_monitor(void *unused1, void *unused2, void *unused3)
{
	bool shutdown_accepted = false;
	bool shutdown_armed = false;
	int previous_pressed = -1;
	int64_t pressed_at = 0;
	int64_t last_report_at = 0;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
	LOG_WRN("POWER STATE DEBUG ACTIVE: logging runtime power-button state");
	LOG_INF("POWER_STATE=ON; waiting for power-on button release before arming shutdown");
#endif

	while (true) {
		int pressed = gpio_pin_get_dt(&power_button);
		int64_t now = k_uptime_get();

		if (pressed < 0) {
			LOG_ERR("Runtime power-button read failed: %d", pressed);
			k_sleep(POWER_POLL_TIME);
			continue;
		}

		if (!shutdown_armed) {
			if (pressed == 0) {
				shutdown_armed = true;
				shutdown_accepted = false;
				previous_pressed = 0;
#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
				LOG_INF("POWER_STATE=ON BUTTON=RELEASED shutdown_armed=yes");
#endif
			}
			k_sleep(POWER_POLL_TIME);
			continue;
		}

		if (pressed != previous_pressed) {
			if (pressed != 0) {
				pressed_at = now;
				shutdown_accepted = false;
#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
				LOG_INF("POWER_STATE=ON BUTTON=PRESSED held=0/%d ms",
					POWER_HOLD_TIME_MS);
#endif
			} else {
#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
				LOG_INF("POWER_STATE=ON BUTTON=RELEASED previous_hold=%lld ms accepted=%s",
					(long long)(pressed_at == 0 ? 0 : now - pressed_at),
					shutdown_accepted ? "yes" : "no");
#endif
				pressed_at = 0;
				shutdown_accepted = false;
			}
			previous_pressed = pressed;
			last_report_at = now;
		}

		if (pressed != 0 && pressed_at != 0) {
			int64_t held_ms = now - pressed_at;

			if (held_ms >= POWER_HOLD_TIME_MS && !shutdown_accepted) {
				shutdown_accepted = true;
#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
				LOG_INF("POWER_OFF=ACCEPTED held=%lld ms; entering System OFF",
					(long long)held_ms);
#endif
				power_control_enter_ultra_low_power();
			}
#if defined(CONFIG_TINYCARDIA_POWER_BUTTON_DEBUG)
			if (!shutdown_accepted &&
			    now - last_report_at >= POWER_DEBUG_HELD_REPORT_MS) {
				LOG_INF("POWER_STATE=ON BUTTON=PRESSED held=%lld/%d ms",
					(long long)held_ms, POWER_HOLD_TIME_MS);
				last_report_at = now;
			}
#endif
		}

		k_sleep(POWER_POLL_TIME);
	}
}

int power_control_start_off_monitor(void)
{
	if (power_monitor_started) {
		return -EALREADY;
	}
	if (!gpio_is_ready_dt(&power_button)) {
		return -ENODEV;
	}

	power_monitor_started = true;
	(void)k_thread_create(&power_monitor_thread, power_monitor_stack,
			      K_THREAD_STACK_SIZEOF(power_monitor_stack),
			      power_control_off_monitor, NULL, NULL, NULL,
			      POWER_MONITOR_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&power_monitor_thread, "power_monitor");

	return 0;
}
