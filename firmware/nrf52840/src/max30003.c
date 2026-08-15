#include "max30003.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(max30003, CONFIG_LOG_DEFAULT_LEVEL);

// important for _INST_ macros (SPI)
// later, SPI_DT_SPEC_INST_GET(0, ...) means get instance 0 of device compat with maxim,max30003
#define DT_DRV_COMPAT maxim_max30003

#define MAX30003_REG_STATUS     0x01
#define MAX30003_REG_EN_INT     0x02
#define MAX30003_REG_MNGR_DYN   0x05
#define MAX30003_REG_SW_RST     0x08
#define MAX30003_REG_SYNCH      0x09
#define MAX30003_REG_FIFO_RST   0x0a
#define MAX30003_REG_INFO       0x0f
#define MAX30003_REG_CNFG_GEN   0x10
#define MAX30003_REG_CNFG_EMUX  0x14
#define MAX30003_REG_CNFG_ECG   0x15
#define MAX30003_REG_CNFG_RTOR  0x1d
#define MAX30003_REG_CNFG_RTOR2 0x1e
#define MAX30003_REG_ECG_FIFO   0x21

#define MAX30003_STATUS_EINT   BIT(23)
#define MAX30003_STATUS_PLLINT BIT(8)
#define MAX30003_STATUS_LDOFF_N (BIT(0) | BIT(1))
#define MAX30003_STATUS_LDOFF_P (BIT(2) | BIT(3))

/* INFO contains a fixed interface pattern and MAX30003 part-identification bits. */
#define MAX30003_INFO_ID_MASK  0xf03000U
#define MAX30003_INFO_ID_VALUE 0x503000U

#define MAX30003_FIFO_ETAG_MASK GENMASK(5, 3)
#define MAX30003_FIFO_ETAG_VALID      0
#define MAX30003_FIFO_ETAG_FAST       1
#define MAX30003_FIFO_ETAG_VALID_LAST 2
#define MAX30003_FIFO_ETAG_FAST_LAST  3
#define MAX30003_FIFO_ETAG_OVF        7
#define MAX30003_FIFO_MAX_READS       33
#define MAX30003_PLL_LOCK_TIMEOUT_MS  100
#define MAX30003_SAMPLE_RATE_HZ        256U

/* Values retained from the validated STM32 configuration. */
#define MAX30003_CNFG_GEN_VALUE  0x081213
#define MAX30003_CNFG_GEN_EN_ECG BIT(19)
#define MAX30003_CNFG_ECG_VALUE  0x425000
#define MAX30003_CNFG_RTOR_VALUE 0x038100
#define MAX30003_EN_INT_VALUE    0x800003

struct max30003_register_info {
	const char *name;
	uint8_t address;
};

struct max30003_register_config {
	const char *name;
	uint8_t address;
	uint32_t value;
};

static const struct max30003_register_info register_table[] = {
	{ "INFO", MAX30003_REG_INFO },
	{ "STATUS", MAX30003_REG_STATUS },
	{ "EN_INT", MAX30003_REG_EN_INT },
	{ "MNGR_DYN", MAX30003_REG_MNGR_DYN },
	{ "SW_RST", MAX30003_REG_SW_RST },
	{ "SYNCH", MAX30003_REG_SYNCH },
	{ "FIFO_RST", MAX30003_REG_FIFO_RST },
	{ "CNFG_GEN", MAX30003_REG_CNFG_GEN },
	{ "CNFG_EMUX", MAX30003_REG_CNFG_EMUX },
	{ "CNFG_ECG", MAX30003_REG_CNFG_ECG },
	{ "CNFG_RTOR", MAX30003_REG_CNFG_RTOR },
	{ "CNFG_RTOR2", MAX30003_REG_CNFG_RTOR2 },
};

static const struct max30003_register_config register_config[] = {
	{ "CNFG_GEN", MAX30003_REG_CNFG_GEN, MAX30003_CNFG_GEN_VALUE },
	{ "CNFG_ECG", MAX30003_REG_CNFG_ECG, MAX30003_CNFG_ECG_VALUE },
	{ "CNFG_RTOR", MAX30003_REG_CNFG_RTOR, MAX30003_CNFG_RTOR_VALUE },
	{ "EN_INT", MAX30003_REG_EN_INT, MAX30003_EN_INT_VALUE },
	{ "MNGR_DYN", MAX30003_REG_MNGR_DYN, 0 },
	{ "CNFG_EMUX", MAX30003_REG_CNFG_EMUX, 0 },
};

static const struct spi_dt_spec max30003_spi = SPI_DT_SPEC_INST_GET(
	0, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));
static const struct gpio_dt_spec max30003_int1 =
	GPIO_DT_SPEC_INST_GET(0, int_gpios);

static struct gpio_callback int1_callback;
static struct k_work fifo_work;
static struct k_work_delayable sample_watchdog_work;
static max30003_sample_handler_t sample_callback;
static void *sample_callback_data;
static uint32_t delivered_sample_count;
static uint32_t timestamp_sample_index;
static uint64_t acquisition_epoch_ms;
static atomic_t monitoring_enabled;
static max30003_lead_status_handler_t lead_status_callback;
static void *lead_status_callback_data;
static max30003_drop_handler_t drop_callback;
static void *drop_callback_data;
static uint32_t pending_dropped_samples;
static enum max30003_lead_status current_lead_status =
	MAX30003_LEAD_STATUS_UNKNOWN;

K_MUTEX_DEFINE(max30003_lock);

static enum max30003_lead_status lead_status_from_register(uint32_t status)
{
	bool negative_off = (status & MAX30003_STATUS_LDOFF_N) != 0U;
	bool positive_off = (status & MAX30003_STATUS_LDOFF_P) != 0U;

	if (negative_off && positive_off) {
		return MAX30003_LEAD_STATUS_BOTH_OFF;
	}
	if (negative_off) {
		return MAX30003_LEAD_STATUS_NEGATIVE_OFF;
	}
	if (positive_off) {
		return MAX30003_LEAD_STATUS_POSITIVE_OFF;
	}
	return MAX30003_LEAD_STATUS_GOOD;
}

static void update_lead_status(enum max30003_lead_status status)
{
	max30003_lead_status_handler_t callback = NULL;
	void *callback_data = NULL;

	k_mutex_lock(&max30003_lock, K_FOREVER);
	if (current_lead_status != status) {
		current_lead_status = status;
		callback = lead_status_callback;
		callback_data = lead_status_callback_data;
	}
	k_mutex_unlock(&max30003_lock);

	if (callback != NULL) {
		callback(status, callback_data);
	}
}

static void report_dropped_samples(uint32_t minimum_dropped)
{
	max30003_drop_handler_t callback;
	void *callback_data;

	k_mutex_lock(&max30003_lock, K_FOREVER);
	callback = drop_callback;
	callback_data = drop_callback_data;
	if (callback == NULL) {
		pending_dropped_samples += minimum_dropped;
	}
	k_mutex_unlock(&max30003_lock);

	if (callback != NULL) {
		callback(minimum_dropped, callback_data);
	}
}

int max30003_write_register(uint8_t reg, uint32_t value)
{
	uint8_t tx_data[4] = {
		(uint8_t)(reg << 1),
		(uint8_t)(value >> 16),
		(uint8_t)(value >> 8),
		(uint8_t)value,
	};
	const struct spi_buf tx_buf = {
		.buf = tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};
	int err;

	k_mutex_lock(&max30003_lock, K_FOREVER);
	err = spi_write_dt(&max30003_spi, &tx);
	k_mutex_unlock(&max30003_lock);

	return err;
}

int max30003_read_register(uint8_t reg, uint32_t *value)
{
	uint8_t tx_data[4] = { (uint8_t)((reg << 1) | 1U), 0xff, 0xff, 0xff };
	uint8_t rx_data[4] = { 0 };
	const struct spi_buf tx_buf = {
		.buf = tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf rx_buf = {
		.buf = rx_data,
		.len = sizeof(rx_data),
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};
	const struct spi_buf_set rx = {
		.buffers = &rx_buf,
		.count = 1,
	};
	int err;

	if (value == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&max30003_lock, K_FOREVER);
	err = spi_transceive_dt(&max30003_spi, &tx, &rx);
	k_mutex_unlock(&max30003_lock);
	if (err < 0) {
		return err;
	}

	*value = ((uint32_t)rx_data[1] << 16) |
		 ((uint32_t)rx_data[2] << 8) |
		 (uint32_t)rx_data[3];

	return 0;
}

int max30003_sanity_check(uint32_t *info)
{
	uint32_t value;
	int err;

	if (info == NULL) {
		return -EINVAL;
	}

	err = max30003_read_register(MAX30003_REG_INFO, &value);
	if (err < 0) {
		return err;
	}

	*info = value;
	if ((value & MAX30003_INFO_ID_MASK) != MAX30003_INFO_ID_VALUE) {
		return -ENODEV;
	}

	return 0;
}

static int reset_and_check_identity(uint32_t *info)
{
	uint32_t ignored;
	int err;

	err = max30003_write_register(MAX30003_REG_SW_RST, 0);
	if (err < 0) {
		return err;
	}
	k_sleep(K_MSEC(10));

	/* The first INFO read after reset is documented as potentially invalid. */
	err = max30003_read_register(MAX30003_REG_INFO, &ignored);
	if (err < 0) {
		return err;
	}

	return max30003_sanity_check(info);
}

static int write_and_verify_registers(void)
{
	uint32_t actual;
	int err;

	for (size_t index = 0; index < ARRAY_SIZE(register_config); ++index) {
		err = max30003_write_register(register_config[index].address,
					       register_config[index].value);
		if (err < 0) {
			LOG_ERR("%s write failed: %d", register_config[index].name, err);
			return err;
		}
	}

	for (size_t index = 0; index < ARRAY_SIZE(register_config); ++index) {
		err = max30003_read_register(register_config[index].address, &actual);
		if (err < 0) {
			LOG_ERR("%s readback failed: %d", register_config[index].name, err);
			return err;
		}
		if (actual != register_config[index].value) {
			LOG_ERR("%s readback mismatch: wrote 0x%06x, read 0x%06x",
				register_config[index].name, register_config[index].value,
				actual);
			return -EIO;
		}
	}

	return 0;
}

static int wait_for_pll_lock(void)
{
	uint32_t status = 0;
	int err;

	for (int elapsed_ms = 0; elapsed_ms < MAX30003_PLL_LOCK_TIMEOUT_MS;
	     ++elapsed_ms) {
		err = max30003_read_register(MAX30003_REG_STATUS, &status);
		if (err < 0) {
			return err;
		}
		if ((status & MAX30003_STATUS_PLLINT) == 0U) {
			return 0;
		}
		k_sleep(K_MSEC(1));
	}

	LOG_ERR("MAX30003 PLL did not lock; STATUS=0x%06x", status);
	return -ETIMEDOUT;
}

static int configure_registers(void)
{
	int err;

	err = write_and_verify_registers();
	if (err < 0) {
		return err;
	}

	err = wait_for_pll_lock();
	if (err < 0) {
		return err;
	}

	return max30003_write_register(MAX30003_REG_SYNCH, 0);
}

static void fifo_work_handler(struct k_work *work)
{
	uint32_t status;
	uint32_t fifo_word;
	uint32_t etag;
	int err;

	ARG_UNUSED(work);
	if (!atomic_get(&monitoring_enabled)) {
		return;
	}

	err = max30003_read_register(MAX30003_REG_STATUS, &status);
	if (err < 0) {
		LOG_ERR("STATUS read failed: %d", err);
		return;
	}
	if ((status & MAX30003_STATUS_EINT) == 0U) {
		return;
	}

	for (size_t index = 0; index < MAX30003_FIFO_MAX_READS; ++index) {
		err = max30003_read_register(MAX30003_REG_ECG_FIFO, &fifo_word);
		if (err < 0) {
			LOG_ERR("ECG FIFO read failed: %d", err);
			return;
		}

		etag = FIELD_GET(MAX30003_FIFO_ETAG_MASK, fifo_word);
		if (etag == MAX30003_FIFO_ETAG_OVF) {
			LOG_WRN("ECG FIFO overflow; resetting FIFO");
			err = max30003_write_register(MAX30003_REG_FIFO_RST, 0);
			if (err < 0) {
				LOG_ERR("ECG FIFO reset failed: %d", err);
			}
			/* The exact loss is unavailable; preserve honest future timestamps. */
			timestamp_sample_index = 0U;
			acquisition_epoch_ms = (uint64_t)k_uptime_get();
			report_dropped_samples(1U);
			return;
		}
		if (etag > MAX30003_FIFO_ETAG_FAST_LAST) {
			return;
		}

		if (sample_callback != NULL) {
			uint32_t timestamp_ms = (uint32_t)(acquisition_epoch_ms +
				((uint64_t)timestamp_sample_index * MSEC_PER_SEC) /
				MAX30003_SAMPLE_RATE_HZ);

			sample_callback(fifo_word, timestamp_ms, sample_callback_data);
		}
		++timestamp_sample_index;
		++delivered_sample_count;
		if (etag == MAX30003_FIFO_ETAG_VALID_LAST ||
		    etag == MAX30003_FIFO_ETAG_FAST_LAST) {
			return;
		}
	}

	LOG_WRN("ECG FIFO did not produce an end tag");
}

static void sample_watchdog_handler(struct k_work *work)
{
	static uint32_t previous_sample_count;
	uint32_t status = 0U;
	int int1_active = 0;
	int err;

	ARG_UNUSED(work);
	if (!atomic_get(&monitoring_enabled)) {
		return;
	}

	err = max30003_read_register(MAX30003_REG_STATUS, &status);
	if (err < 0) {
		LOG_ERR("MAX30003 health-check STATUS read failed: %d", err);
	} else {
		update_lead_status(lead_status_from_register(status));
	}

	if (delivered_sample_count == previous_sample_count) {
		int1_active = gpio_pin_get_dt(&max30003_int1);
		if (err < 0) {
			/* The STATUS read error was already logged above. */
		} else if (int1_active < 0) {
			LOG_ERR("MAX30003 health-check INT1 read failed: %d", int1_active);
		} else if ((status & MAX30003_STATUS_EINT) != 0U) {
			LOG_WRN("ECG FIFO is ready but no samples arrived; STATUS=0x%06x, INT1 active=%d; polling once",
				status, int1_active);
			(void)k_work_submit(&fifo_work);
		} else {
			LOG_WRN("No ECG samples in %u ms; STATUS=0x%06x, INT1 active=%d",
				CONFIG_TINYCARDIA_MAX30003_STATUS_POLL_MS, status,
				int1_active);
		}
	}

	previous_sample_count = delivered_sample_count;
	(void)k_work_reschedule(&sample_watchdog_work,
				K_MSEC(CONFIG_TINYCARDIA_MAX30003_STATUS_POLL_MS));
}

static void int1_handler(const struct device *port, struct gpio_callback *callback,
			 gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);

	(void)k_work_submit(&fifo_work);
}

int max30003_init(max30003_sample_handler_t handler, void *user_data)
{
	uint32_t info = 0;
	int err;

	if (!spi_is_ready_dt(&max30003_spi) || !gpio_is_ready_dt(&max30003_int1)) {
		LOG_ERR("SPI or INT1 GPIO device is not ready");
		return -ENODEV;
	}
	LOG_INF("SPI bus %s ready: mode 0, %u Hz", max30003_spi.bus->name,
		(unsigned int)max30003_spi.config.frequency);

	sample_callback = handler;
	sample_callback_data = user_data;
	delivered_sample_count = 0;
	timestamp_sample_index = 0U;
	acquisition_epoch_ms = (uint64_t)k_uptime_get();
	atomic_set(&monitoring_enabled, 1);
	k_work_init(&fifo_work, fifo_work_handler);
	k_work_init_delayable(&sample_watchdog_work, sample_watchdog_handler);

	err = gpio_pin_configure_dt(&max30003_int1, GPIO_INPUT);
	if (err < 0) {
		return err;
	}

	err = reset_and_check_identity(&info);
	if (err < 0) {
		if (err == -ENODEV) {
			LOG_ERR("Invalid MAX30003 INFO 0x%06x; check power, CSB, SCLK, MOSI, and MISO",
				info);
		}
		return err;
	}
	LOG_INF("MAX30003 connected: INFO=0x%06x, revision=%u", info,
		(unsigned int)FIELD_GET(GENMASK(19, 16), info));

	err = configure_registers();
	if (err < 0) {
		return err;
	}
	LOG_INF("MAX30003 register readback passed; ECG configured for 256 sps");
	timestamp_sample_index = 0U;
	acquisition_epoch_ms = (uint64_t)k_uptime_get();

	gpio_init_callback(&int1_callback, int1_handler, BIT(max30003_int1.pin));
	err = gpio_add_callback(max30003_int1.port, &int1_callback);
	if (err < 0) {
		return err;
	}
	err = gpio_pin_interrupt_configure_dt(&max30003_int1,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (err < 0) {
		return err;
	}

	/* Service an interrupt that was already asserted before the edge was enabled. */
	err = gpio_pin_get_dt(&max30003_int1);
	if (err > 0) {
		(void)k_work_submit(&fifo_work);
	} else if (err < 0) {
		return err;
	}
	(void)k_work_schedule(&sample_watchdog_work,
			      K_MSEC(CONFIG_TINYCARDIA_MAX30003_STATUS_POLL_MS));

	return 0;
}

int max30003_set_monitoring(bool enabled)
{
	struct k_work_sync fifo_sync;
	struct k_work_sync watchdog_sync;
	uint32_t cnfg_gen = enabled ? MAX30003_CNFG_GEN_VALUE :
		(MAX30003_CNFG_GEN_VALUE & ~MAX30003_CNFG_GEN_EN_ECG);
	int err;
	int first_err = 0;

	if ((atomic_get(&monitoring_enabled) != 0) == enabled) {
		return 0;
	}

	if (!enabled) {
		int transition_err;
		int rollback_err = 0;

		atomic_clear(&monitoring_enabled);
		err = gpio_pin_interrupt_configure_dt(&max30003_int1, GPIO_INT_DISABLE);
		if (err < 0 && first_err == 0) {
			first_err = err;
		}
		(void)k_work_cancel_sync(&fifo_work, &fifo_sync);
		(void)k_work_cancel_delayable_sync(&sample_watchdog_work, &watchdog_sync);
		err = max30003_write_register(MAX30003_REG_CNFG_GEN, cnfg_gen);
		if (err < 0 && first_err == 0) {
			first_err = err;
		}
		err = max30003_write_register(MAX30003_REG_FIFO_RST, 0);
		if (err < 0 && first_err == 0) {
			first_err = err;
		}
		update_lead_status(MAX30003_LEAD_STATUS_UNKNOWN);
		if (first_err == 0) {
			return 0;
		}

		/* Restore the previous ON state so callers never observe a half-stop. */
		transition_err = first_err;
		rollback_err = max30003_write_register(MAX30003_REG_CNFG_GEN,
						     MAX30003_CNFG_GEN_VALUE);
		if (rollback_err == 0) {
			rollback_err = max30003_write_register(MAX30003_REG_FIFO_RST, 0);
		}
		if (rollback_err == 0) {
			rollback_err = max30003_write_register(MAX30003_REG_SYNCH, 0);
		}
		if (rollback_err == 0) {
			timestamp_sample_index = 0U;
			acquisition_epoch_ms = (uint64_t)k_uptime_get();
			atomic_set(&monitoring_enabled, 1);
			rollback_err = gpio_pin_interrupt_configure_dt(
				&max30003_int1, GPIO_INT_EDGE_TO_ACTIVE);
		}
		if (rollback_err == 0) {
			(void)k_work_schedule(&sample_watchdog_work,
				K_MSEC(CONFIG_TINYCARDIA_MAX30003_STATUS_POLL_MS));
		} else {
			atomic_clear(&monitoring_enabled);
			(void)gpio_pin_interrupt_configure_dt(&max30003_int1,
						      GPIO_INT_DISABLE);
			(void)max30003_write_register(MAX30003_REG_CNFG_GEN,
				MAX30003_CNFG_GEN_VALUE & ~MAX30003_CNFG_GEN_EN_ECG);
			LOG_ERR("MAX30003 stop rollback failed: %d", rollback_err);
		}
		return transition_err;
	}

	err = max30003_write_register(MAX30003_REG_CNFG_GEN, cnfg_gen);
	if (err < 0) {
		return err;
	}
	err = max30003_write_register(MAX30003_REG_FIFO_RST, 0);
	if (err < 0) {
		goto enable_failed;
	}
	err = max30003_write_register(MAX30003_REG_SYNCH, 0);
	if (err < 0) {
		goto enable_failed;
	}

	timestamp_sample_index = 0U;
	acquisition_epoch_ms = (uint64_t)k_uptime_get();
	atomic_set(&monitoring_enabled, 1);
	err = gpio_pin_interrupt_configure_dt(&max30003_int1, GPIO_INT_EDGE_TO_ACTIVE);
	if (err < 0) {
		atomic_clear(&monitoring_enabled);
		(void)max30003_write_register(MAX30003_REG_CNFG_GEN,
			MAX30003_CNFG_GEN_VALUE & ~MAX30003_CNFG_GEN_EN_ECG);
		return err;
	}
	err = gpio_pin_get_dt(&max30003_int1);
	if (err > 0) {
		(void)k_work_submit(&fifo_work);
	} else if (err < 0) {
		atomic_clear(&monitoring_enabled);
		(void)gpio_pin_interrupt_configure_dt(&max30003_int1, GPIO_INT_DISABLE);
		(void)max30003_write_register(MAX30003_REG_CNFG_GEN,
			MAX30003_CNFG_GEN_VALUE & ~MAX30003_CNFG_GEN_EN_ECG);
		return err;
	}
	(void)k_work_schedule(&sample_watchdog_work,
			      K_MSEC(CONFIG_TINYCARDIA_MAX30003_STATUS_POLL_MS));

	return 0;

enable_failed:
	(void)max30003_write_register(MAX30003_REG_CNFG_GEN,
		MAX30003_CNFG_GEN_VALUE & ~MAX30003_CNFG_GEN_EN_ECG);
	return err;
}

bool max30003_is_monitoring(void)
{
	return atomic_get(&monitoring_enabled) != 0;
}

void max30003_set_lead_status_handler(max30003_lead_status_handler_t handler,
				      void *user_data)
{
	enum max30003_lead_status status;

	k_mutex_lock(&max30003_lock, K_FOREVER);
	lead_status_callback = handler;
	lead_status_callback_data = user_data;
	status = current_lead_status;
	k_mutex_unlock(&max30003_lock);

	if (handler != NULL) {
		handler(status, user_data);
	}
}

void max30003_set_drop_handler(max30003_drop_handler_t handler, void *user_data)
{
	uint32_t pending;

	k_mutex_lock(&max30003_lock, K_FOREVER);
	drop_callback = handler;
	drop_callback_data = user_data;
	pending = pending_dropped_samples;
	pending_dropped_samples = 0U;
	k_mutex_unlock(&max30003_lock);

	if (handler != NULL && pending > 0U) {
		handler(pending, user_data);
	}
}

int max30003_dump_registers(void)
{
	uint32_t value;
	int err;

	for (size_t index = 0; index < ARRAY_SIZE(register_table); ++index) {
		err = max30003_read_register(register_table[index].address, &value);
		if (err < 0) {
			return err;
		}
		LOG_INF("%-10s (0x%02x) = 0x%06x", register_table[index].name,
			register_table[index].address, value);
	}

	return 0;
}
