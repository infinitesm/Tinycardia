#include "max30003.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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

#define MAX30003_STATUS_EINT    BIT(23)
#define MAX30003_FIFO_ETAG_MASK GENMASK(5, 3)
#define MAX30003_FIFO_ETAG_VALID      0
#define MAX30003_FIFO_ETAG_FAST       1
#define MAX30003_FIFO_ETAG_VALID_LAST 2
#define MAX30003_FIFO_ETAG_FAST_LAST  3
#define MAX30003_FIFO_ETAG_OVF        7
#define MAX30003_FIFO_MAX_READS       33

/* Values retained from the validated STM32 configuration. */
#define MAX30003_CNFG_GEN_VALUE  0x081213
#define MAX30003_CNFG_ECG_VALUE  0x425000
#define MAX30003_CNFG_RTOR_VALUE 0x038100
#define MAX30003_EN_INT_VALUE    0x800003

struct max30003_register_info {
	const char *name;
	uint8_t address;
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

static const struct spi_dt_spec max30003_spi = SPI_DT_SPEC_INST_GET(
	0, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));
static const struct gpio_dt_spec max30003_int1 =
	GPIO_DT_SPEC_INST_GET(0, int_gpios);

static struct gpio_callback int1_callback;
static struct k_work fifo_work;
static max30003_sample_handler_t sample_callback;
static void *sample_callback_data;

K_MUTEX_DEFINE(max30003_lock);

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

int max30003_sanity_check(uint8_t *info)
{
	uint32_t value;
	int err;

	if (info == NULL) {
		return -EINVAL;
	}

	err = max30003_read_register(MAX30003_REG_INFO, &value);
	if (err == 0) {
		*info = (uint8_t)value;
	}

	return err;
}

static int configure_registers(void)
{
	int err;

	err = max30003_write_register(MAX30003_REG_SW_RST, 0);
	if (err < 0) {
		return err;
	}
	k_sleep(K_MSEC(10));

	err = max30003_write_register(MAX30003_REG_CNFG_GEN,
				       MAX30003_CNFG_GEN_VALUE);
	if (err < 0) {
		return err;
	}
	err = max30003_write_register(MAX30003_REG_CNFG_ECG,
				       MAX30003_CNFG_ECG_VALUE);
	if (err < 0) {
		return err;
	}
	err = max30003_write_register(MAX30003_REG_CNFG_RTOR,
				       MAX30003_CNFG_RTOR_VALUE);
	if (err < 0) {
		return err;
	}
	err = max30003_write_register(MAX30003_REG_EN_INT,
				       MAX30003_EN_INT_VALUE);
	if (err < 0) {
		return err;
	}
	err = max30003_write_register(MAX30003_REG_MNGR_DYN, 0);
	if (err < 0) {
		return err;
	}
	err = max30003_write_register(MAX30003_REG_CNFG_EMUX, 0);
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
			(void)max30003_write_register(MAX30003_REG_FIFO_RST, 0);
			return;
		}
		if (etag > MAX30003_FIFO_ETAG_FAST_LAST) {
			return;
		}

		if (sample_callback != NULL) {
			sample_callback(fifo_word, sample_callback_data);
		}
		if (etag == MAX30003_FIFO_ETAG_VALID_LAST ||
		    etag == MAX30003_FIFO_ETAG_FAST_LAST) {
			return;
		}
	}

	LOG_WRN("ECG FIFO did not produce an end tag");
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
	uint8_t info;
	int err;

	if (!spi_is_ready_dt(&max30003_spi) || !gpio_is_ready_dt(&max30003_int1)) {
		return -ENODEV;
	}

	sample_callback = handler;
	sample_callback_data = user_data;
	k_work_init(&fifo_work, fifo_work_handler);

	err = gpio_pin_configure_dt(&max30003_int1, GPIO_INPUT);
	if (err < 0) {
		return err;
	}

	err = configure_registers();
	if (err < 0) {
		return err;
	}

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

	err = max30003_sanity_check(&info);
	if (err < 0) {
		return err;
	}
	LOG_INF("MAX30003 INFO low byte: 0x%02x", info);

	/* Service an interrupt that was already asserted before the edge was enabled. */
	err = gpio_pin_get_dt(&max30003_int1);
	if (err > 0) {
		(void)k_work_submit(&fifo_work);
	} else if (err < 0) {
		return err;
	}

	return 0;
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
