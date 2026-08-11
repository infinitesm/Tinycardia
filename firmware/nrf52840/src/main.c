#include "max30003.h"
#include "power_control.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

// BT advertising data
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)), // "discoverable, BLE only"

	// complete list of 16-bit service UUIDs
	// currently only the Device Information Service UUID is included, later we include our own services
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
};

// define BT scan response data
// sent when another BLE device actively scans it
static const struct bt_data sd[] = {
	// complete device name, device name, how many bytes long it is
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

// work handler callback for zephyr to start BLE advertising
static void advertising_work_handler(struct k_work *work)
{
	// start the advertisement
	// BT_LE_ADV_CONN_FAST_1 = "i am here i am here i am here yes connect to me i am here"
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd,
				  ARRAY_SIZE(sd));

	if (err) {
		printk("BLE advertising failed (err %d)\n", err);
		return;
	}

	printk("BLE advertising as %s\n", CONFIG_BT_DEVICE_NAME);
}

// define the k_work macro, we will submit this at startup to use it.
K_WORK_DEFINE(advertising_work, advertising_work_handler);

static void connected(struct bt_conn *conn, uint8_t err)
{
	char address[BT_ADDR_LE_STR_LEN];

	if (err) {
		printk("BLE connection failed (err 0x%02x)\n", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), address, sizeof(address));
	printk("BLE connected: %s\n", address);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("BLE disconnected (reason 0x%02x)\n", reason);
}

static void recycled(void)
{
	(void)k_work_submit(&advertising_work);
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

int main(void)
{
	int err;

	err = power_control_wait_for_on();
	if (err < 0) {
		printk("Power-button initialization failed (err %d)\n", err);
		return 0;
	}

	printk("Tinycardia v2 booted\n");

	err = max30003_init(NULL, NULL);
	if (err < 0) {
		printk("MAX30003 initialization failed (err %d)\n", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth initialization failed (err %d)\n", err);
		return 0;
	}

	(void)k_work_submit(&advertising_work);

	return 0;
}
