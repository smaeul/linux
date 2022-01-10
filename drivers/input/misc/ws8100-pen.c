// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WS8100 BLE Pen Driver
 *
 * Copyright (c) 2022 Samuel Holland <samuel@sholland.org>
 */

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/spi/spi.h>

#define WS8100_IRQ_PEN_VERSION_INFO	0x12
#define WS8100_IRQ_PEN_BATTERY_INFO	0x13
#define WS8100_IRQ_PEN_OTA_STATUS	0x14
#define WS8100_IRQ_OTA_STATUS_A		0x45
#define WS8100_IRQ_OTA_STATUS_B		0x46
#define WS8100_IRQ_OTA_STATUS_C		0x47
#define WS8100_IRQ_INPUT		0x66
#define WS8100_IRQ_SCAN_COMPLETE	0x77

#define WS8100_ADDRESS_LEN		6
#define WS8100_BATTERY_LEN		2
#define WS8100_COMMAND_LEN		3
#define WS8100_READ_STATUS_LEN		2
#define WS8100_VERSION_LEN		3

#define WS8100_MAX_RX_LEN		10
#define WS8100_MAX_SCAN_RESULTS		50
#define WS8100_MAX_XFER_RETRY		3

#define WS8100_RX_DATA_OFFSET		2
#define WS8100_RX_OVERHEAD		3

struct ws8100_pen {
	struct completion	pen_battery_read_complete;
	struct completion	pen_version_read_complete;
	struct completion	scan_complete;
	struct gpio_desc	*cs_gpio;
	struct gpio_desc	*host_state_gpio;
	struct gpio_desc	*reset_gpio;
	struct input_dev	*input;
	struct mutex		scan_results_mutex;
	struct mutex		xfer_mutex;

	ktime_t	pen_battery_time;
	bool	pen_present;

	u8	controller_version[WS8100_VERSION_LEN];
	u8	pen_address[WS8100_ADDRESS_LEN + 1];
	u8	pen_battery[WS8100_BATTERY_LEN];
	u8	pen_version[WS8100_VERSION_LEN];
	u8	scan_results[WS8100_MAX_SCAN_RESULTS][WS8100_ADDRESS_LEN];
	u8	scan_result_count;
};

/*
 * All transfers follow the same pattern:
 *	1) Command, always 3 bytes:
 *		[r/w]	Always 0x50 for "writes", 0x51 for "reads"
 *		[op]	Determines what the command does
 *		[sum]	Sum to 0x00
 *	2) Transmitted data, optional, variable length:
 *		[data]	Zero or more bytes, depending on the command
 *		[sum]	Sum to 0x00, only present if there is data
 *	3) Received data, variable length:
 *		[r/w]	Copy of the byte from the command
 *		[op]	Copy of the byte from the command
 *		[data]	Zero or more bytes, depending on the command
 *		[sum]	Sum to 0x00
 */
struct ws8100_cmd {
	u8	command[WS8100_COMMAND_LEN];
	u8	rx_data_len;
	u16	tx_data_len;
};

#define WS8100_CMD(_name, _cmd0, _cmd1, _tx, _rx) \
	static const struct ws8100_cmd _name = { \
		.command	= { _cmd0, _cmd1, (0x100 - _cmd0 - _cmd1) }, \
		.tx_data_len	= _tx, \
		.rx_data_len	= _rx, \
	}

/*						 r/w    op   tx rx */
WS8100_CMD(ws8100_cmd_read_from_pen,		0x51, 0x11,   2, 2);
WS8100_CMD(ws8100_cmd_get_pen_version,		0x51, 0x12,   0, 3);
WS8100_CMD(ws8100_cmd_get_pen_battery,		0x51, 0x13,   0, 2);
WS8100_CMD(ws8100_cmd_get_pen_ota_status,	0x51, 0x14,   0, 1);
WS8100_CMD(ws8100_cmd_start_pen_ota,		0x50, 0x16,   0, 0);
WS8100_CMD(ws8100_cmd_set_suspend_mode,		0x50, 0x22,   2, 1);
WS8100_CMD(ws8100_cmd_get_controller_version,	0x51, 0x33,   0, 3);
WS8100_CMD(ws8100_cmd_start_ota,		0x50, 0x44,   7, 1);
WS8100_CMD(ws8100_cmd_write_ota_data,		0x50, 0x45, 504, 1);
WS8100_CMD(ws8100_cmd_cancel_ota,		0x50, 0x46,   0, 0);
WS8100_CMD(ws8100_cmd_get_ota_status,		0x51, 0x47,   0, 1);
WS8100_CMD(ws8100_cmd_stop_ota,			0x50, 0x48,   0, 0);
WS8100_CMD(ws8100_cmd_set_default_pen_address,	0x50, 0x55,   7, 1);
WS8100_CMD(ws8100_cmd_get_default_pen_address,	0x51, 0x55,   0, 7);
WS8100_CMD(ws8100_cmd_get_input,		0x51, 0x66,   0, 1);
WS8100_CMD(ws8100_cmd_start_scan,		0x50, 0x77,   0, 0);
WS8100_CMD(ws8100_cmd_get_scan_result_count,	0x51, 0x78,   0, 1);
WS8100_CMD(ws8100_cmd_get_irq,			0x51, 0x88,   0, 1);
WS8100_CMD(ws8100_cmd_get_scan_result,		0x51, 0xc0,   0, 6);

static int ws8100_pen_xfer(struct spi_device *spi,
			   const struct ws8100_cmd *c,
			   u8 *tx_data, u8 *rx_data)
{
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	u8 rx_len = WS8100_RX_OVERHEAD + c->rx_data_len;
	u8 rx_buf[WS8100_MAX_RX_LEN];
	int ret, try;
	u8 i, sum;
	struct spi_transfer xfers[3] = {
		{
			.tx_buf	= c->command,
			.len	= WS8100_COMMAND_LEN,
			.delay	= { .value = c->tx_data_len ? 0 : 50 },
		},
		{
			.tx_buf	= tx_data,
			.len	= c->tx_data_len,
			.delay	= { .value = 50 },
		},
		{
			.rx_buf	= rx_buf,
			.len	= rx_len,
		},
	};

	if ((c->tx_data_len && !tx_data) || (c->rx_data_len && !rx_data))
		return -EINVAL;
	if (rx_len > sizeof(rx_buf))
		return -EINVAL;

	mutex_lock(&pen->xfer_mutex);

	dev_dbg(&spi->dev, "send [%*ph] [%*ph]\n",
		WS8100_COMMAND_LEN, c->command, c->tx_data_len, tx_data);

	for (try = 0; try < WS8100_MAX_XFER_RETRY; ++try) {
		if (try)
			msleep(50);

		gpiod_set_value_cansleep(pen->cs_gpio, 1);
		msleep(10);
		ret = spi_sync_transfer(spi, xfers, ARRAY_SIZE(xfers));
		gpiod_set_value_cansleep(pen->cs_gpio, 0);
		if (ret)
			goto out_unlock;

		dev_dbg(&spi->dev, "recv [%*ph]\n", rx_len, rx_buf);

		if (rx_buf[0] != c->command[0])
			continue;
		for (i = 0, sum = 0; i < rx_len; ++i)
			sum += rx_buf[i];
		if (sum)
			continue;

		memcpy(rx_data, &rx_buf[WS8100_RX_DATA_OFFSET], c->rx_data_len);
		goto out_unlock;
	}

	ret = -EBADMSG;

out_unlock:
	mutex_unlock(&pen->xfer_mutex);

	return ret;
}

static void ws8100_pen_input(struct spi_device *spi)
{
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	int code = 0, ret, tool = 0;
	u8 input_status;

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_get_input, NULL, &input_status);
	if (ret) {
		dev_warn(&spi->dev, "Failed to read input: %d\n", ret);
		return;
	}

	dev_dbg(&spi->dev, "Got input 0x%02x\n", input_status);

	switch (input_status) {
	case 0x02: /* short press */
		code = BTN_STYLUS;
		tool = BTN_TOOL_RUBBER;
		break;
	case 0x20: /* long press */
		code = KEY_BACK;
		tool = BTN_TOOL_RUBBER;
		break;
	case 0x22: /* double press */
		code = KEY_MACRO1;
		tool = BTN_TOOL_RUBBER;
		break;
	case 0x23: /* leading edge */
		tool = BTN_TOOL_RUBBER;
		break;

	case 0x04: /* short press */
		code = BTN_STYLUS2;
		tool = BTN_TOOL_PEN;
		break;
	case 0x40: /* long press */
		code = KEY_FORWARD;
		tool = BTN_TOOL_PEN;
		break;
	case 0x42: /* double press */
		code = KEY_MACRO2;
		tool = BTN_TOOL_PEN;
		break;
	case 0x43: /* leading edge */
		tool = BTN_TOOL_PEN;
		break;

	case 0x01: /* short press */
		code = BTN_STYLUS3;
		break;
	case 0x10: /* long press */
		code = KEY_SLEEP;
		break;
	case 0x08: /* double press */
		code = KEY_MACRO3;
		break;

	default:
		dev_warn(&spi->dev, "Unknown input: 0x%02x\n", input_status);
		break;
	}

	if (code) {
		input_report_key(pen->input, code, 1);
		input_sync(pen->input);
		input_report_key(pen->input, code, 0);
		if (tool)
			input_report_key(pen->input, tool, 0);
	} else {
		if (tool)
			input_report_key(pen->input, tool, 1);
	}
	input_event(pen->input, EV_MSC, MSC_SCAN, input_status);
	input_sync(pen->input);
}

static void ws8100_pen_scan_complete(struct spi_device *spi)
{
	struct ws8100_cmd result_cmd = ws8100_cmd_get_scan_result;
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	u8 i, scan_result_count;
	int ret;

	mutex_lock(&pen->scan_results_mutex);

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_get_scan_result_count,
			      NULL, &scan_result_count);
	if (ret)
		goto out_unlock;

	pen->scan_result_count = min_t(u8, scan_result_count, WS8100_MAX_SCAN_RESULTS);
	for (i = 0; i < pen->scan_result_count; ++i) {
		ret = ws8100_pen_xfer(spi, &result_cmd,
				      NULL, pen->scan_results[i]);
		if (ret)
			goto out_unlock;

		result_cmd.command[1]++; /* increment the result index */
		result_cmd.command[2]--; /* decrement the checksum to match */
	}

out_unlock:
	if (ret) {
		dev_warn(&spi->dev, "Failed to read scan results: %d\n", ret);
		pen->scan_result_count = 0;
	}

	mutex_unlock(&pen->scan_results_mutex);
}

static irqreturn_t ws8100_pen_irq(int irq, void *data)
{
	struct spi_device *spi = data;
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	u8 irq_status;
	int ret;

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_get_irq, NULL, &irq_status);
	if (ret) {
		dev_warn(&spi->dev, "Failed to read IRQ: %d\n", ret);
		return IRQ_HANDLED;
	}

	dev_dbg(&spi->dev, "Got IRQ 0x%02x\n", irq_status);

	switch (irq_status) {
	case WS8100_IRQ_PEN_VERSION_INFO:
		ws8100_pen_xfer(spi, &ws8100_cmd_get_pen_version,
				NULL, pen->pen_version);
		complete(&pen->pen_version_read_complete);
		break;
	case WS8100_IRQ_PEN_BATTERY_INFO:
		ws8100_pen_xfer(spi, &ws8100_cmd_get_pen_battery,
				NULL, pen->pen_battery);
		complete(&pen->pen_battery_read_complete);
		break;
	case WS8100_IRQ_INPUT:
		ws8100_pen_input(spi);
		break;
	case WS8100_IRQ_SCAN_COMPLETE:
		ws8100_pen_scan_complete(spi);
		complete(&pen->scan_complete);
		break;
	default:
		dev_warn(&spi->dev, "Unknown IRQ: 0x%02x\n", irq_status);
		break;
	}

	return IRQ_HANDLED;
}

static int ws8100_pen_get_controller_version(struct spi_device *spi)
{
	struct ws8100_pen *pen = spi_get_drvdata(spi);

	return ws8100_pen_xfer(spi, &ws8100_cmd_get_controller_version,
			       NULL, pen->controller_version);
}

static int ws8100_pen_get_pen_address(struct spi_device *spi)
{
	struct ws8100_pen *pen = spi_get_drvdata(spi);

	return ws8100_pen_xfer(spi, &ws8100_cmd_get_default_pen_address,
			       NULL, pen->pen_address);
}

static int ws8100_pen_get_pen_battery(struct spi_device *spi)
{
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	u8 read_status[WS8100_READ_STATUS_LEN];
	u8 battery_data[2] = { 0x02, 0xfe };
	int ret;

	reinit_completion(&pen->pen_battery_read_complete);

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_read_from_pen,
			      battery_data, read_status);
	if (ret)
		return ret;

	/* Fail early if no pen is connected. */
	if (!read_status[0])
		return -ENXIO;

	/* Usually takes 100-200 ms. */
	if (!wait_for_completion_timeout(&pen->pen_battery_read_complete,
					 msecs_to_jiffies(500)))
		return -ETIMEDOUT;

	return 0;
}

static int ws8100_pen_get_pen_version(struct spi_device *spi)
{
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	u8 read_status[WS8100_READ_STATUS_LEN];
	u8 version_data[2] = { 0x01, 0xff };
	int ret;

	reinit_completion(&pen->pen_version_read_complete);

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_read_from_pen,
			      version_data, read_status);
	if (ret)
		return ret;

	/* Fail early if no pen is connected. */
	if (!read_status[0])
		return -ENXIO;

	/* Usually takes 100-200 ms. */
	if (!wait_for_completion_timeout(&pen->pen_version_read_complete,
					 msecs_to_jiffies(500)))
		return -ETIMEDOUT;

	return 0;
}

static int ws8100_pen_scan(struct spi_device *spi)
{
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	int ret;

	reinit_completion(&pen->scan_complete);

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_start_scan, NULL, NULL);
	if (ret)
		return ret;

	/* Scans usually take about 12 seconds, but sometimes longer. */
	if (!wait_for_completion_interruptible_timeout(&pen->scan_complete,
						       msecs_to_jiffies(20000)))
		return -ETIMEDOUT;

	return 0;
}

static const enum power_supply_property ws8100_pen_psy_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
};

static int ws8100_pen_psy_get_proprty(struct power_supply *psy,
				      enum power_supply_property psp,
				      union power_supply_propval *val)
{
	struct spi_device *spi = power_supply_get_drvdata(psy);
	struct ws8100_pen *pen = spi_get_drvdata(spi);
	ktime_t now = ktime_get();
	int ret;


	if (ktime_after(now, pen->pen_battery_time)) {
		ret = ws8100_pen_get_pen_battery(spi);
		if (ret && ret != -ENXIO)
			return ret;

		pen->pen_battery_time = ktime_add_ns(now, NSEC_PER_SEC);
		pen->pen_present = !ret;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = pen->pen_battery[0]
			? POWER_SUPPLY_STATUS_CHARGING
			: POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = pen->pen_present;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = pen->pen_battery[1];
		return 0;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc ws8100_pen_psy_desc = {
	.name			= "ws8100_pen",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= ws8100_pen_psy_properties,
	.num_properties		= ARRAY_SIZE(ws8100_pen_psy_properties),
	.get_property		= ws8100_pen_psy_get_proprty,
};

static ssize_t controller_version_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);
	struct spi_device *spi = to_spi_device(dev);
	int ret;

	ret = ws8100_pen_get_controller_version(spi);
	if (ret)
		return ret;

	return sprintf(buf, "%.*s\n", WS8100_VERSION_LEN,
		       pen->controller_version);
}

static DEVICE_ATTR_RO(controller_version);

static ssize_t host_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
		       gpiod_get_value_cansleep(pen->host_state_gpio));
}

static ssize_t host_state_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);
	bool state;
	int ret;

	ret = kstrtobool(buf, &state);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(pen->host_state_gpio, state);

	return count;
}

static DEVICE_ATTR_RW(host_state);

static ssize_t pen_address_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);
	struct spi_device *spi = to_spi_device(dev);
	int ret;

	ret = ws8100_pen_get_pen_address(spi);
	if (ret)
		return ret;

	return sprintf(buf, "%pM\n", pen->pen_address);
}

static ssize_t pen_address_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	u8 address[WS8100_ADDRESS_LEN + 1];
	u8 status, sum;
	int i, ret;

	ret = sscanf(buf, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
		     &address[0], &address[1], &address[2],
		     &address[3], &address[4], &address[5]);
	if (ret != WS8100_ADDRESS_LEN)
		return -EINVAL;

	for (i = 0, sum = 0; i < WS8100_ADDRESS_LEN; ++i)
		sum += address[i];
	address[WS8100_ADDRESS_LEN] = -sum;

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_set_default_pen_address,
			      address, &status);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(pen_address);

static ssize_t pen_battery_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);
	struct spi_device *spi = to_spi_device(dev);
	int ret;

	ret = ws8100_pen_get_pen_battery(spi);
	if (ret)
		return ret;

	return sprintf(buf, "%*ph\n", WS8100_BATTERY_LEN, pen->pen_battery);
}

static DEVICE_ATTR_RO(pen_battery);

static ssize_t pen_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);
	struct spi_device *spi = to_spi_device(dev);
	int ret;

	ret = ws8100_pen_get_pen_version(spi);
	if (ret)
		return ret;

	return sprintf(buf, "%.*s\n", WS8100_VERSION_LEN, pen->pen_version);
}

static DEVICE_ATTR_RO(pen_version);

static ssize_t scan_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);
	ssize_t count;
	u8 i;

	mutex_lock(&pen->scan_results_mutex);

	count = sprintf(buf, "Scan Results (%d)\n-----------------\n",
			pen->scan_result_count);

	for (i = 0; i < pen->scan_result_count; ++i)
		count += sprintf(buf, "%pM\n", pen->scan_results[i]);

	mutex_unlock(&pen->scan_results_mutex);

	return count;
}

static ssize_t scan_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	int ret;

	ret = ws8100_pen_scan(spi);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(scan);

static ssize_t suspend_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	u8 mode[2], status;
	int ret;

	ret = kstrtou8(buf, 10, &mode[0]);
	if (ret)
		return ret;
	mode[1] = -mode[0];

	ret = ws8100_pen_xfer(spi, &ws8100_cmd_set_suspend_mode, mode, &status);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_WO(suspend_mode);

static struct attribute *ws8100_pen_attributes[] = {
	&dev_attr_controller_version.attr,
	&dev_attr_host_state.attr,
	&dev_attr_pen_address.attr,
	&dev_attr_pen_battery.attr,
	&dev_attr_pen_version.attr,
	&dev_attr_scan.attr,
	&dev_attr_suspend_mode.attr,
	NULL
};

static const struct attribute_group *ws8100_pen_groups[] = {
	&(const struct attribute_group) {
		.attrs = ws8100_pen_attributes,
	},
	NULL
};

static int ws8100_pen_suspend(struct device *dev)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);

	gpiod_set_value_cansleep(pen->host_state_gpio, 0);

	return 0;
}

static int ws8100_pen_resume(struct device *dev)
{
	struct ws8100_pen *pen = dev_get_drvdata(dev);

	gpiod_set_value_cansleep(pen->host_state_gpio, 1);

	return 0;
}

static int ws8100_pen_probe(struct spi_device *spi)
{
	struct power_supply_config psy_cfg = { .drv_data = spi };
	struct device *dev = &spi->dev;
	struct power_supply *psy;
	struct ws8100_pen *pen;
	int code, ret;

	pen = devm_kzalloc(dev, sizeof(*pen), GFP_KERNEL);
	if (!pen)
		return -ENOMEM;

	spi_set_drvdata(spi, pen);
	init_completion(&pen->pen_battery_read_complete);
	init_completion(&pen->pen_version_read_complete);
	init_completion(&pen->scan_complete);
	mutex_init(&pen->scan_results_mutex);
	mutex_init(&pen->xfer_mutex);

	pen->cs_gpio = devm_gpiod_get(dev, "cs", GPIOD_OUT_LOW);
	if (IS_ERR(pen->cs_gpio))
		return dev_err_probe(dev, PTR_ERR(pen->cs_gpio),
				     "Failed to get %s GPIO\n", "cs");

	pen->host_state_gpio = devm_gpiod_get(dev, "host_state", GPIOD_OUT_HIGH);
	if (IS_ERR(pen->host_state_gpio))
		return dev_err_probe(dev, PTR_ERR(pen->host_state_gpio),
				     "Failed to get %s GPIO\n", "host_state");

	pen->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pen->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pen->reset_gpio),
				     "Failed to get %s GPIO\n", "reset");

	pen->input = devm_input_allocate_device(dev);
	if (!pen->input)
		return -ENOMEM;

	input_set_drvdata(pen->input, spi);

	pen->input->name = "ws8100_pen";
	pen->input->phys = "ws8100_pen/input0";
	pen->input->id.bustype = BUS_BLUETOOTH;

	__set_bit(EV_KEY, pen->input->evbit);
	__set_bit(KEY_SLEEP, pen->input->keybit);
	__set_bit(KEY_BACK, pen->input->keybit);
	__set_bit(KEY_FORWARD, pen->input->keybit);
	__set_bit(BTN_TOOL_PEN, pen->input->keybit);
	__set_bit(BTN_TOOL_RUBBER, pen->input->keybit);
	__set_bit(BTN_STYLUS3, pen->input->keybit);
	__set_bit(BTN_STYLUS, pen->input->keybit);
	__set_bit(BTN_STYLUS2, pen->input->keybit);
	for (code = KEY_MACRO1; code < KEY_MACRO4; ++code)
		__set_bit(code, pen->input->keybit);

	__set_bit(EV_MSC, pen->input->evbit);
	__set_bit(MSC_SCAN, pen->input->mscbit);

	ret = input_register_device(pen->input);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register input\n");

	psy = devm_power_supply_register(dev, &ws8100_pen_psy_desc, &psy_cfg);
	if (IS_ERR(psy))
		return PTR_ERR(psy);

	power_supply_powers(psy, &pen->input->dev);

	ret = devm_request_threaded_irq(dev, spi->irq, NULL, ws8100_pen_irq,
					IRQF_ONESHOT, dev_name(dev), spi);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	/* Ensure the controller is fully reset. */
	msleep(50);

	gpiod_set_value_cansleep(pen->reset_gpio, 0);

	return 0;
}

static void ws8100_pen_shutdown(struct spi_device *spi)
{
	ws8100_pen_suspend(&spi->dev);
}

static const struct spi_device_id ws8100_pen_id_table[] = {
	{ .name = "pinenote-ws8100-pen" },
	{}
};
MODULE_DEVICE_TABLE(spi, ws8100_pen_id_table);

static const struct of_device_id ws8100_pen_of_match[] = {
	{ .compatible = "pine64,pinenote-ws8100-pen" },
	{}
};
MODULE_DEVICE_TABLE(of, ws8100_pen_of_match);

static SIMPLE_DEV_PM_OPS(ws8100_pen_pm_ops,
			 ws8100_pen_suspend, ws8100_pen_resume);

static struct spi_driver ws8100_pen_driver = {
	.id_table	= ws8100_pen_id_table,
	.probe		= ws8100_pen_probe,
	.shutdown	= ws8100_pen_shutdown,
	.driver		= {
		.name		= "ws8100_pen",
		.of_match_table	= ws8100_pen_of_match,
		.dev_groups	= ws8100_pen_groups,
		.pm		= &ws8100_pen_pm_ops,
	},
};

module_spi_driver(ws8100_pen_driver);

MODULE_DESCRIPTION("WS8100 BLE Pen Driver");
MODULE_AUTHOR("Samuel Holland <samuel@sholland.org>");
MODULE_LICENSE("GPL");
