/* SPDX-License-Identifier: MIT */

#include "ble_service.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ble_service, CONFIG_LOG_DEFAULT_LEVEL);

#define BLE_ECG_PACKET_WAIT K_MSEC(45)
#define BLE_DROP_LOG_INTERVAL 64U
#define BLE_ADVERTISING_RETRY K_SECONDS(1)

enum battery_service_attribute_index {
	BATTERY_SERVICE_ATTRIBUTE,
	BATTERY_LEVEL_DECLARATION_ATTRIBUTE,
	BATTERY_LEVEL_VALUE_ATTRIBUTE,
	BATTERY_LEVEL_CCC_ATTRIBUTE,
};

enum tinycardia_service_attribute_index {
	TINYCARDIA_SERVICE_ATTRIBUTE,
	TINYCARDIA_ECG_DECLARATION_ATTRIBUTE,
	TINYCARDIA_ECG_VALUE_ATTRIBUTE,
	TINYCARDIA_ECG_CCC_ATTRIBUTE,
	TINYCARDIA_INFERENCE_DECLARATION_ATTRIBUTE,
	TINYCARDIA_INFERENCE_VALUE_ATTRIBUTE,
	TINYCARDIA_INFERENCE_CCC_ATTRIBUTE,
	TINYCARDIA_STATUS_DECLARATION_ATTRIBUTE,
	TINYCARDIA_STATUS_VALUE_ATTRIBUTE,
	TINYCARDIA_STATUS_CCC_ATTRIBUTE,
	TINYCARDIA_CONTROL_DECLARATION_ATTRIBUTE,
	TINYCARDIA_CONTROL_VALUE_ATTRIBUTE,
};

struct queued_ecg_sample {
	int32_t sample;
	uint32_t timestamp_ms;
	bool loss_already_counted;
};

struct queued_inference {
	uint8_t packet[TINYCARDIA_INFERENCE_PACKET_SIZE];
	uint32_t connection_generation;
};

static struct bt_uuid_128 tinycardia_service_uuid =
	BT_UUID_INIT_128(TINYCARDIA_UUID_SERVICE_VAL);
static struct bt_uuid_128 ecg_stream_uuid =
	BT_UUID_INIT_128(TINYCARDIA_UUID_ECG_STREAM_VAL);
static struct bt_uuid_128 inference_uuid =
	BT_UUID_INIT_128(TINYCARDIA_UUID_INFERENCE_VAL);
static struct bt_uuid_128 status_uuid =
	BT_UUID_INIT_128(TINYCARDIA_UUID_STATUS_VAL);
static struct bt_uuid_128 control_uuid =
	BT_UUID_INIT_128(TINYCARDIA_UUID_CONTROL_VAL);

static struct tinycardia_protocol_state protocol_state;
static struct tinycardia_ble_callbacks application_callbacks;
static void *application_callback_data;
static struct bt_conn *active_connection;
static enum tinycardia_lead_status current_lead_status =
	TINYCARDIA_LEAD_STATUS_UNKNOWN;
static bool ecg_subscribed;
static bool inference_subscribed;
static bool status_subscribed;
static bool battery_subscribed;
static bool battery_level_valid;
static uint8_t battery_level;
static uint32_t ecg_sequence;
static uint32_t connection_generation;
static uint32_t monitoring_started_ms;
static bool control_error_latched;
static bool explicit_error;

static atomic_t service_initialized;
static atomic_t samples_acquired;
static atomic_t samples_dropped;
static atomic_t inference_count;

K_MUTEX_DEFINE(service_lock);
K_MUTEX_DEFINE(control_lock);
K_MSGQ_DEFINE(ecg_stream_queue, sizeof(struct queued_ecg_sample),
	      CONFIG_TINYCARDIA_BLE_ECG_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(inference_queue, sizeof(struct queued_inference),
	      CONFIG_TINYCARDIA_BLE_INFERENCE_QUEUE_DEPTH, 4);

static struct k_work_q ble_work_queue;
static struct k_work_delayable ecg_tx_work;
static struct k_work inference_tx_work;
static struct k_work status_notify_work;
static struct k_work battery_notify_work;
static struct k_work_delayable advertising_work;
K_THREAD_STACK_DEFINE(ble_work_queue_stack, CONFIG_TINYCARDIA_BLE_THREAD_STACK_SIZE);

static struct bt_conn *connection_ref(void)
{
	struct bt_conn *connection = NULL;

	k_mutex_lock(&service_lock, K_FOREVER);
	if (active_connection != NULL) {
		connection = bt_conn_ref(active_connection);
	}
	k_mutex_unlock(&service_lock);

	return connection;
}

static struct bt_conn *inference_connection_ref(uint32_t queued_generation)
{
	struct bt_conn *connection = NULL;

	k_mutex_lock(&service_lock, K_FOREVER);
	if (active_connection != NULL && inference_subscribed &&
	    queued_generation == connection_generation) {
		connection = bt_conn_ref(active_connection);
	}
	k_mutex_unlock(&service_lock);

	return connection;
}

static uint8_t connection_max_ecg_samples(struct bt_conn *connection)
{
	return connection == NULL ? 0U :
		tinycardia_ecg_samples_for_att_mtu(bt_gatt_get_mtu(connection));
}

static void record_dropped_samples(uint32_t count)
{
	uint32_t previous;
	uint32_t total;

	if (count == 0U) {
		return;
	}

	previous = (uint32_t)atomic_add(&samples_dropped, (atomic_val_t)count);
	total = previous + count;
	if (previous == 0U || previous / BLE_DROP_LOG_INTERVAL != total / BLE_DROP_LOG_INTERVAL) {
		LOG_WRN("ECG samples dropped: %u total", total);
		if (atomic_get(&service_initialized)) {
			(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);
		}
	}
}

static void get_status_snapshot(struct tinycardia_status *status)
{
	k_mutex_lock(&service_lock, K_FOREVER);
	status->lead_status = current_lead_status;
	status->operating_state = tinycardia_get_operating_state(&protocol_state);
	k_mutex_unlock(&service_lock);

	status->uptime_s = (uint32_t)(k_uptime_get() / MSEC_PER_SEC);
	status->samples_acquired = (uint32_t)atomic_get(&samples_acquired);
	status->samples_dropped = (uint32_t)atomic_get(&samples_dropped);
	status->inference_count = (uint32_t)atomic_get(&inference_count);
}

static ssize_t read_battery_level(struct bt_conn *connection,
				  const struct bt_gatt_attr *attribute,
				  void *buffer, uint16_t length, uint16_t offset)
{
	uint8_t level;

	ARG_UNUSED(attribute);

	k_mutex_lock(&service_lock, K_FOREVER);
	if (!battery_level_valid) {
		k_mutex_unlock(&service_lock);
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	level = battery_level;
	k_mutex_unlock(&service_lock);

	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
				 &level, sizeof(level));
}

static ssize_t read_device_status(struct bt_conn *connection,
				  const struct bt_gatt_attr *attribute,
				  void *buffer, uint16_t length, uint16_t offset)
{
	struct tinycardia_status status;
	uint8_t packet[TINYCARDIA_STATUS_PACKET_SIZE];
	int err;

	get_status_snapshot(&status);
	err = tinycardia_encode_status_packet(packet, sizeof(packet), &status);
	if (err < 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
				 packet, sizeof(packet));
}

static int apply_device_control(enum tinycardia_control_command command)
{
	struct tinycardia_protocol_state proposed_state;
	struct tinycardia_transport_state transport;
	bool monitoring_changed;
	bool streaming_changed;
	int err;

	k_mutex_lock(&control_lock, K_FOREVER);
	k_mutex_lock(&service_lock, K_FOREVER);
	proposed_state = protocol_state;
	transport.connected = active_connection != NULL;
	transport.ecg_subscribed = ecg_subscribed;
	transport.max_ecg_samples = connection_max_ecg_samples(active_connection);
	err = tinycardia_apply_control(&proposed_state, command, &transport);
	if (err < 0) {
		k_mutex_unlock(&service_lock);
		k_mutex_unlock(&control_lock);
		return err;
	}

	monitoring_changed = proposed_state.monitoring != protocol_state.monitoring;
	streaming_changed = proposed_state.streaming != protocol_state.streaming;
	if (!monitoring_changed) {
		protocol_state = proposed_state;
		k_mutex_unlock(&service_lock);
		goto state_applied;
	}
	k_mutex_unlock(&service_lock);

	if (monitoring_changed && application_callbacks.set_monitoring != NULL) {
		err = application_callbacks.set_monitoring(proposed_state.monitoring,
						   application_callback_data);
		if (err < 0) {
			k_mutex_lock(&service_lock, K_FOREVER);
			protocol_state.error = true;
			control_error_latched = true;
			k_mutex_unlock(&service_lock);
			k_mutex_unlock(&control_lock);
			(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);
			return err;
		}
	}
	k_mutex_lock(&service_lock, K_FOREVER);
	if (control_error_latched) {
		control_error_latched = false;
	}
	proposed_state.error = explicit_error;
	protocol_state = proposed_state;
	if (proposed_state.monitoring) {
		monitoring_started_ms = k_uptime_get_32();
	}
	k_mutex_unlock(&service_lock);

state_applied:
	if (streaming_changed) {
		k_msgq_purge(&ecg_stream_queue);
	}
	if (monitoring_changed && !proposed_state.monitoring) {
		k_msgq_purge(&inference_queue);
	}
	k_mutex_unlock(&control_lock);
	(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);

	if (monitoring_changed) {
		LOG_INF("Monitoring %s", proposed_state.monitoring ? "started" : "stopped");
	}
	if (streaming_changed) {
		LOG_INF("ECG streaming %s", proposed_state.streaming ? "started" : "stopped");
	}

	return 0;
}

static ssize_t write_device_control(struct bt_conn *connection,
				    const struct bt_gatt_attr *attribute,
				    const void *buffer, uint16_t length,
				    uint16_t offset, uint8_t flags)
{
	enum tinycardia_control_command command;
	int err;

	ARG_UNUSED(connection);
	ARG_UNUSED(attribute);

	if ((flags & BT_GATT_WRITE_FLAG_CMD) != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
	}
	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	err = tinycardia_decode_control(buffer, length, &command);
	if (err == -EMSGSIZE) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (err < 0) {
		LOG_WRN("Invalid Device Control value");
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	err = apply_device_control(command);
	if (err == -EAGAIN) {
		return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
	}
	if (err == -EACCES) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (err < 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return length;
}

static void battery_ccc_changed(const struct bt_gatt_attr *attribute, uint16_t value)
{
	ARG_UNUSED(attribute);

	k_mutex_lock(&service_lock, K_FOREVER);
	battery_subscribed = value == BT_GATT_CCC_NOTIFY;
	k_mutex_unlock(&service_lock);
}

static void ecg_ccc_changed(const struct bt_gatt_attr *attribute, uint16_t value)
{
	bool streaming_stopped = false;

	ARG_UNUSED(attribute);

	k_mutex_lock(&service_lock, K_FOREVER);
	ecg_subscribed = value == BT_GATT_CCC_NOTIFY;
	if (!ecg_subscribed && protocol_state.streaming) {
		protocol_state.streaming = false;
		streaming_stopped = true;
	}
	k_mutex_unlock(&service_lock);

	LOG_INF("ECG notifications %s", ecg_subscribed ? "subscribed" : "unsubscribed");
	if (streaming_stopped) {
		k_msgq_purge(&ecg_stream_queue);
		(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);
		LOG_INF("ECG streaming stopped after subscription removal");
	}
}

static void inference_ccc_changed(const struct bt_gatt_attr *attribute, uint16_t value)
{
	bool subscribed;

	ARG_UNUSED(attribute);

	k_mutex_lock(&service_lock, K_FOREVER);
	subscribed = value == BT_GATT_CCC_NOTIFY;
	inference_subscribed = subscribed;
	k_mutex_unlock(&service_lock);
	if (!subscribed) {
		k_msgq_purge(&inference_queue);
	}
}

static void status_ccc_changed(const struct bt_gatt_attr *attribute, uint16_t value)
{
	ARG_UNUSED(attribute);

	k_mutex_lock(&service_lock, K_FOREVER);
	status_subscribed = value == BT_GATT_CCC_NOTIFY;
	k_mutex_unlock(&service_lock);
	if (value == BT_GATT_CCC_NOTIFY) {
		(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);
	}
}

BT_GATT_SERVICE_DEFINE(battery_service,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
	BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_battery_level, NULL, NULL),
	BT_GATT_CCC(battery_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

BT_GATT_SERVICE_DEFINE(tinycardia_service,
	BT_GATT_PRIMARY_SERVICE(&tinycardia_service_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&ecg_stream_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ecg_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&inference_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(inference_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_device_status, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&control_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_device_control, NULL));

static bool notification_is_enabled(bool subscribed)
{
	bool enabled;

	k_mutex_lock(&service_lock, K_FOREVER);
	enabled = active_connection != NULL && subscribed;
	k_mutex_unlock(&service_lock);

	return enabled;
}

static void battery_notify_handler(struct k_work *work)
{
	struct bt_conn *connection;
	uint8_t level;
	bool valid;
	bool subscribed;
	int err;

	ARG_UNUSED(work);

	k_mutex_lock(&service_lock, K_FOREVER);
	valid = battery_level_valid;
	subscribed = battery_subscribed;
	level = battery_level;
	k_mutex_unlock(&service_lock);
	if (!valid || !notification_is_enabled(subscribed)) {
		return;
	}

	connection = connection_ref();
	if (connection == NULL) {
		return;
	}
	err = bt_gatt_notify(connection,
			     &battery_service.attrs[BATTERY_LEVEL_VALUE_ATTRIBUTE],
			     &level, sizeof(level));
	bt_conn_unref(connection);
	if (err < 0) {
		LOG_WRN("Battery notification failed: %d", err);
	}
}

static void status_notify_handler(struct k_work *work)
{
	struct tinycardia_status status;
	struct bt_conn *connection;
	uint8_t packet[TINYCARDIA_STATUS_PACKET_SIZE];
	bool subscribed;
	int err;

	ARG_UNUSED(work);

	k_mutex_lock(&service_lock, K_FOREVER);
	subscribed = status_subscribed;
	k_mutex_unlock(&service_lock);
	if (!notification_is_enabled(subscribed)) {
		return;
	}

	get_status_snapshot(&status);
	err = tinycardia_encode_status_packet(packet, sizeof(packet), &status);
	if (err < 0) {
		return;
	}
	connection = connection_ref();
	if (connection == NULL) {
		return;
	}
	err = bt_gatt_notify(connection,
			     &tinycardia_service.attrs[TINYCARDIA_STATUS_VALUE_ATTRIBUTE],
			     packet, sizeof(packet));
	bt_conn_unref(connection);
	if (err < 0) {
		LOG_WRN("Device Status notification failed: %d", err);
	}
}

static void inference_tx_handler(struct k_work *work)
{
	struct queued_inference queued;

	ARG_UNUSED(work);

	while (k_msgq_get(&inference_queue, &queued, K_NO_WAIT) == 0) {
		struct bt_conn *connection;
		int err;

		connection = inference_connection_ref(queued.connection_generation);
		if (connection == NULL) {
			continue;
		}
		err = bt_gatt_notify(
			connection,
			&tinycardia_service.attrs[TINYCARDIA_INFERENCE_VALUE_ATTRIBUTE],
			queued.packet, sizeof(queued.packet));
		bt_conn_unref(connection);
		if (err < 0) {
			LOG_WRN("Inference notification failed: %d", err);
		}
	}
}

static uint8_t streaming_sample_target(struct bt_conn **connection)
{
	uint8_t target = 0U;

	*connection = NULL;
	k_mutex_lock(&service_lock, K_FOREVER);
	if (protocol_state.streaming && ecg_subscribed && active_connection != NULL) {
		*connection = bt_conn_ref(active_connection);
		target = connection_max_ecg_samples(active_connection);
	}
	k_mutex_unlock(&service_lock);

	return target;
}

static void schedule_ecg_tx(uint8_t target)
{
	uint32_t queued_count = k_msgq_num_used_get(&ecg_stream_queue);
	k_timeout_t delay = queued_count >= target ? K_NO_WAIT : BLE_ECG_PACKET_WAIT;

	if (target > 0U && queued_count > 0U) {
		(void)k_work_reschedule_for_queue(&ble_work_queue, &ecg_tx_work, delay);
	}
}

static void ecg_tx_handler(struct k_work *work)
{
	struct queued_ecg_sample queued;
	struct bt_conn *connection;
	int32_t samples[TINYCARDIA_ECG_MAX_SAMPLES];
	uint8_t packet[TINYCARDIA_ECG_FULL_PACKET_SIZE];
	uint8_t sample_count = 0U;
	uint8_t uncounted_sample_count = 0U;
	uint8_t target;
	uint32_t first_timestamp = 0U;
	size_t packet_size;
	int err;

	ARG_UNUSED(work);

	target = streaming_sample_target(&connection);
	if (target == 0U || connection == NULL) {
		k_msgq_purge(&ecg_stream_queue);
		return;
	}

	while (sample_count < target &&
	       k_msgq_get(&ecg_stream_queue, &queued, K_NO_WAIT) == 0) {
		if (sample_count == 0U) {
			first_timestamp = queued.timestamp_ms;
		}
		if (!queued.loss_already_counted) {
			++uncounted_sample_count;
		}
		samples[sample_count++] = queued.sample;
	}
	if (sample_count == 0U) {
		bt_conn_unref(connection);
		return;
	}

	err = tinycardia_encode_ecg_packet(packet, sizeof(packet), ecg_sequence++,
					 first_timestamp, samples, sample_count, &packet_size);
	if (err == 0) {
		err = bt_gatt_notify(
			connection,
			&tinycardia_service.attrs[TINYCARDIA_ECG_VALUE_ATTRIBUTE],
			packet, (uint16_t)packet_size);
	}
	bt_conn_unref(connection);
	if (err < 0) {
		/* Do not count a sample twice if analysis had already lost it. */
		record_dropped_samples(uncounted_sample_count);
		LOG_WRN("ECG notification failed: %d", err);
	}

	target = streaming_sample_target(&connection);
	if (connection != NULL) {
		bt_conn_unref(connection);
	}
	schedule_ecg_tx(target);
}

static const struct bt_data advertising_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, TINYCARDIA_UUID_SERVICE_VAL),
};

static const struct bt_data scan_response_data[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1U),
};

static int start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising_data,
				  ARRAY_SIZE(advertising_data), scan_response_data,
				  ARRAY_SIZE(scan_response_data));

	if (err == 0) {
		LOG_INF("Advertising as %s", CONFIG_BT_DEVICE_NAME);
	} else if (err != -EALREADY) {
		LOG_ERR("Advertising failed: %d", err);
	}

	return err == -EALREADY ? 0 : err;
}

static void advertising_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (start_advertising() < 0) {
		(void)k_work_reschedule_for_queue(&ble_work_queue, &advertising_work,
						 BLE_ADVERTISING_RETRY);
	}
}

static void connected(struct bt_conn *connection, uint8_t err)
{
	char address[BT_ADDR_LE_STR_LEN];

	if (err != 0U) {
		LOG_WRN("Connection failed: 0x%02x", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(connection), address, sizeof(address));
	k_mutex_lock(&service_lock, K_FOREVER);
	if (active_connection != NULL) {
		bt_conn_unref(active_connection);
	}
	active_connection = bt_conn_ref(connection);
	++connection_generation;
	protocol_state.streaming = false;
	k_mutex_unlock(&service_lock);
	LOG_INF("Connected: %s, ATT MTU %u, ECG samples/packet %u", address,
		(unsigned int)bt_gatt_get_mtu(connection),
		(unsigned int)connection_max_ecg_samples(connection));
}

static void disconnected(struct bt_conn *connection, uint8_t reason)
{
	ARG_UNUSED(connection);

	k_mutex_lock(&service_lock, K_FOREVER);
	tinycardia_state_on_disconnect(&protocol_state);
	++connection_generation;
	ecg_subscribed = false;
	inference_subscribed = false;
	status_subscribed = false;
	battery_subscribed = false;
	if (active_connection != NULL) {
		bt_conn_unref(active_connection);
		active_connection = NULL;
	}
	k_mutex_unlock(&service_lock);
	k_msgq_purge(&ecg_stream_queue);
	k_msgq_purge(&inference_queue);
	LOG_INF("Disconnected: reason 0x%02x; monitoring continues", reason);
}

static void recycled(void)
{
	(void)k_work_reschedule_for_queue(&ble_work_queue, &advertising_work, K_NO_WAIT);
}

BT_CONN_CB_DEFINE(tinycardia_connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

static void mtu_updated(struct bt_conn *connection, uint16_t tx, uint16_t rx)
{
	LOG_INF("ATT MTU updated: tx=%u rx=%u, ECG samples/packet=%u",
		(unsigned int)tx, (unsigned int)rx,
		(unsigned int)connection_max_ecg_samples(connection));
}

static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = mtu_updated,
};

int tinycardia_ble_init(const struct tinycardia_ble_callbacks *callbacks,
			void *user_data, bool monitoring_enabled)
{
	int err;

	if (callbacks == NULL || callbacks->set_monitoring == NULL) {
		return -EINVAL;
	}
	if (atomic_get(&service_initialized)) {
		return -EALREADY;
	}

	application_callbacks = *callbacks;
	application_callback_data = user_data;
	protocol_state.monitoring = monitoring_enabled;
	protocol_state.streaming = false;
	protocol_state.error = false;
	control_error_latched = false;
	explicit_error = false;
	current_lead_status = TINYCARDIA_LEAD_STATUS_UNKNOWN;
	ecg_sequence = 0U;
	monitoring_started_ms = k_uptime_get_32();
	k_work_queue_init(&ble_work_queue);
	k_work_queue_start(&ble_work_queue, ble_work_queue_stack,
			   K_THREAD_STACK_SIZEOF(ble_work_queue_stack),
			   CONFIG_TINYCARDIA_BLE_THREAD_PRIORITY, NULL);
	(void)k_thread_name_set(k_work_queue_thread_get(&ble_work_queue), "ble_tx");
	k_work_init_delayable(&ecg_tx_work, ecg_tx_handler);
	k_work_init(&inference_tx_work, inference_tx_handler);
	k_work_init(&status_notify_work, status_notify_handler);
	k_work_init(&battery_notify_work, battery_notify_handler);
	k_work_init_delayable(&advertising_work, advertising_handler);

	bt_gatt_cb_register(&gatt_callbacks);
	err = bt_enable(NULL);
	if (err < 0) {
		return err;
	}
	atomic_set(&service_initialized, 1);
	err = start_advertising();
	if (err < 0) {
		(void)k_work_reschedule_for_queue(&ble_work_queue, &advertising_work,
						 BLE_ADVERTISING_RETRY);
	}

	LOG_INF("Tinycardia BLE protocol v%u initialized",
		(unsigned int)TINYCARDIA_PROTOCOL_VERSION);
	return 0;
}

bool tinycardia_ble_is_monitoring(void)
{
	bool monitoring;

	k_mutex_lock(&service_lock, K_FOREVER);
	monitoring = protocol_state.monitoring;
	k_mutex_unlock(&service_lock);

	return monitoring;
}

void tinycardia_ble_ecg_sample(int32_t sample, uint32_t timestamp_ms,
			       bool processing_preserved)
{
	struct queued_ecg_sample queued = {
		.sample = sample,
		.timestamp_ms = timestamp_ms,
		.loss_already_counted = !processing_preserved,
	};
	struct bt_conn *connection;
	bool dropped = !processing_preserved;
	uint8_t target;

	atomic_inc(&samples_acquired);
	target = streaming_sample_target(&connection);
	if (connection != NULL) {
		bt_conn_unref(connection);
	}
	if (target > 0U && k_msgq_put(&ecg_stream_queue, &queued, K_NO_WAIT) < 0) {
		dropped = true;
	}
	if (dropped) {
		record_dropped_samples(1U);
	}
	if (target > 0U) {
		schedule_ecg_tx(target);
	}
}

void tinycardia_ble_record_dropped_samples(uint32_t count)
{
	record_dropped_samples(count);
}

int tinycardia_ble_inference_publish(uint32_t timestamp_ms,
				     enum tinycardia_classification classification,
				     enum tinycardia_signal_quality signal_quality,
				     uint16_t confidence)
{
	struct queued_inference queued;
	struct tinycardia_inference_result result;
	bool subscribed;
	int err;

	if (classification < TINYCARDIA_CLASSIFICATION_NORMAL ||
	    classification > TINYCARDIA_CLASSIFICATION_UNKNOWN ||
	    signal_quality < TINYCARDIA_SIGNAL_QUALITY_GOOD ||
	    signal_quality > TINYCARDIA_SIGNAL_QUALITY_UNKNOWN ||
	    (confidence > 10000U && confidence != TINYCARDIA_CONFIDENCE_UNAVAILABLE)) {
		return -EINVAL;
	}

	/*
	 * Serialize publication against controls so STOP_MONITORING cannot leave a
	 * late inference queued after it purges the inference transport queue.
	 */
	k_mutex_lock(&control_lock, K_FOREVER);
	k_mutex_lock(&service_lock, K_FOREVER);
	if (!protocol_state.monitoring || protocol_state.error ||
	    !tinycardia_timestamp_is_in_session(timestamp_ms, monitoring_started_ms)) {
		k_mutex_unlock(&service_lock);
		k_mutex_unlock(&control_lock);
		return -EACCES;
	}
	subscribed = active_connection != NULL && inference_subscribed;
	queued.connection_generation = connection_generation;
	k_mutex_unlock(&service_lock);

	result.inference_id = (uint32_t)atomic_inc(&inference_count);
	result.timestamp_ms = timestamp_ms;
	result.classification = classification;
	result.signal_quality = signal_quality;
	result.confidence = confidence;
	err = tinycardia_encode_inference_packet(queued.packet, sizeof(queued.packet), &result);
	if (err < 0) {
		k_mutex_unlock(&control_lock);
		return err;
	}
	(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);

	if (!subscribed) {
		k_mutex_unlock(&control_lock);
		return 0;
	}
	if (k_msgq_put(&inference_queue, &queued, K_NO_WAIT) < 0) {
		LOG_WRN("Inference notification queue full");
		k_mutex_unlock(&control_lock);
		return -ENOSPC;
	}
	(void)k_work_submit_to_queue(&ble_work_queue, &inference_tx_work);
	k_mutex_unlock(&control_lock);

	return 0;
}

int tinycardia_ble_battery_set_level(uint8_t percentage)
{
	bool changed;

	if (percentage > 100U) {
		return -ERANGE;
	}

	k_mutex_lock(&service_lock, K_FOREVER);
	changed = !battery_level_valid || battery_level != percentage;
	battery_level = percentage;
	battery_level_valid = true;
	k_mutex_unlock(&service_lock);
	if (changed && atomic_get(&service_initialized)) {
		(void)k_work_submit_to_queue(&ble_work_queue, &battery_notify_work);
	}

	return 0;
}

int tinycardia_ble_status_set_lead(enum tinycardia_lead_status lead_status)
{
	bool changed;

	if (lead_status < TINYCARDIA_LEAD_STATUS_GOOD ||
	    lead_status > TINYCARDIA_LEAD_STATUS_UNKNOWN) {
		return -EINVAL;
	}

	k_mutex_lock(&service_lock, K_FOREVER);
	changed = current_lead_status != lead_status;
	current_lead_status = lead_status;
	k_mutex_unlock(&service_lock);
	if (changed && atomic_get(&service_initialized)) {
		LOG_INF("Lead status changed: %u", (unsigned int)lead_status);
		(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);
	}

	return 0;
}

void tinycardia_ble_status_set_error(bool error)
{
	bool changed;

	k_mutex_lock(&service_lock, K_FOREVER);
	explicit_error = error;
	changed = protocol_state.error != (explicit_error || control_error_latched);
	protocol_state.error = explicit_error || control_error_latched;
	k_mutex_unlock(&service_lock);
	if (changed && atomic_get(&service_initialized)) {
		(void)k_work_submit_to_queue(&ble_work_queue, &status_notify_work);
	}
}
