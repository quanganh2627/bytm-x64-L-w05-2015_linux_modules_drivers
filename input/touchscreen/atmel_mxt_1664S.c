/*
 * Atmel maXTouch Touchscreen driver
 *
 * Copyright (C) 2010 Samsung Electronics Co.Ltd
 * Copyright (C) 2011 Atmel Corporation
 * Author: Joonyoung Shim <jy0922.shim@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/acpi_gpio.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <asm/intel_vlv2.h>

#define SUPPORT_STYLUS	0
#define MXT_FORCE_BOOTLOADER	1
#define BOOTLOADER_1664_1188	1
#define MXT1664S_FAMILY_ID	0xa2

/* Firmware files */
#define MXT_FW_NAME		"maxtouch.fw"
#define MXT_CFG_NAME		"maxtouch.cfg"
#define MXT_CFG_MAGIC		"OBP_RAW V1"

/* Registers */
#define MXT_OBJECT_START	0x07
#define MXT_OBJECT_SIZE		6
#define MXT_INFO_CHECKSUM_SIZE	3
#define MXT_MAX_BLOCK_WRITE	256

/* Object types */
#define MXT_DEBUG_DIAGNOSTIC_T37	37
#define MXT_GEN_MESSAGE_T5		5
#define MXT_GEN_COMMAND_T6		6
#define MXT_GEN_POWER_T7		7
#define MXT_GEN_ACQUIRE_T8		8
#define MXT_GEN_DATASOURCE_T53		53
#define MXT_TOUCH_MULTI_T9		9
#define MXT_TOUCH_KEYARRAY_T15		15
#define MXT_TOUCH_PROXIMITY_T23		23
#define MXT_TOUCH_PROXKEY_T52		52
#define MXT_PROCI_GRIPFACE_T20		20
#define MXT_PROCG_NOISE_T22		22
#define MXT_PROCI_ONETOUCH_T24		24
#define MXT_PROCI_TWOTOUCH_T27		27
#define MXT_PROCI_GRIP_T40		40
#define MXT_PROCI_PALM_T41		41
#define MXT_PROCI_TOUCHSUPPRESSION_T42	42
#define MXT_PROCI_STYLUS_T47		47
#define MXT_PROCG_NOISESUPPRESSION_T48	48
#define MXT_SPT_COMMSCONFIG_T18		18
#define MXT_SPT_GPIOPWM_T19		19
#define MXT_SPT_SELFTEST_T25		25
#define MXT_SPT_CTECONFIG_T28		28
#define MXT_SPT_USERDATA_T38		38
#define MXT_SPT_DIGITIZER_T43		43
#define MXT_SPT_MESSAGECOUNT_T44	44
#define MXT_SPT_CTECONFIG_T46		46
#define MXT_SPT_NOISESUPPRESSION_T48	48
#define MXT_PROCI_ACTIVE_STYLUS_T63	63

/* MXT_GEN_MESSAGE_T5 object */
#define MXT_RPTID_NOMSG		0xff

/* MXT_GEN_COMMAND_T6 field */
#define MXT_COMMAND_RESET	0
#define MXT_COMMAND_BACKUPNV	1
#define MXT_COMMAND_CALIBRATE	2
#define MXT_COMMAND_REPORTALL	3
#define MXT_COMMAND_DIAGNOSTIC	5

/* Define for T6 status byte */
#define MXT_T6_STATUS_RESET	(1 << 7)
#define MXT_T6_STATUS_OFL	(1 << 6)
#define MXT_T6_STATUS_SIGERR	(1 << 5)
#define MXT_T6_STATUS_CAL	(1 << 4)
#define MXT_T6_STATUS_CFGERR	(1 << 3)
#define MXT_T6_STATUS_COMSERR	(1 << 2)

/* MXT_GEN_POWER_T7 field */
struct t7_config {
	u8 idle;
	u8 active;
} __packed;

#define MXT_POWER_CFG_RUN		0
#define MXT_POWER_CFG_DEEPSLEEP		1

/* MXT_TOUCH_MULTI_T9 field */
#define MXT_T9_ORIENT		9
#define MXT_T9_RANGE		18

/* MXT_TOUCH_MULTI_T9 status */
#define MXT_T9_UNGRIP		(1 << 0)
#define MXT_T9_SUPPRESS		(1 << 1)
#define MXT_T9_AMP		(1 << 2)
#define MXT_T9_VECTOR		(1 << 3)
#define MXT_T9_MOVE		(1 << 4)
#define MXT_T9_RELEASE		(1 << 5)
#define MXT_T9_PRESS		(1 << 6)
#define MXT_T9_DETECT		(1 << 7)

struct t9_range {
	u16 x;
	u16 y;
} __packed;

/* MXT_TOUCH_MULTI_T9 orient */
#define MXT_T9_ORIENT_SWITCH	(1 << 0)

/* T15 Key array */
int mxt_t15_keys[] = { };

static unsigned long mxt_t15_keystatus;

/* MXT_SPT_COMMSCONFIG_T18 */
#define MXT_COMMS_CTRL		0
#define MXT_COMMS_CMD		1

/* Define for MXT_GEN_COMMAND_T6 */
#define MXT_BOOT_VALUE		0xa5
#define MXT_RESET_VALUE		0x01
#define MXT_BACKUP_VALUE	0x55

/* Define for MXT_PROCI_TOUCHSUPPRESSION_T42 */
#define MXT_T42_MSG_TCHSUP	(1 << 0)

/* T63 Stylus */
#define MXT_STYLUS_PRESS	(1 << 0)
#define MXT_STYLUS_RELEASE	(1 << 1)
#define MXT_STYLUS_MOVE		(1 << 2)
#define MXT_STYLUS_SUPPRESS	(1 << 3)

#define MXT_STYLUS_DETECT	(1 << 4)
#define MXT_STYLUS_TIP		(1 << 5)
#define MXT_STYLUS_ERASER	(1 << 6)
#define MXT_STYLUS_BARREL	(1 << 7)

#define MXT_STYLUS_PRESSURE_MASK	0x3F

/* Delay times */
#define MXT_BACKUP_TIME		25	/* msec */
#define MXT_RESET_TIME		200	/* msec */
#define MXT_RESET_NOCHGREAD	400	/* msec */
#define MXT_FWRESET_TIME	1000	/* msec */
#define MXT_WAKEUP_TIME		25	/* msec */

/* Command to unlock bootloader */
#define MXT_UNLOCK_CMD_MSB	0xaa
#define MXT_UNLOCK_CMD_LSB	0xdc

/* Bootloader mode status */
#define MXT_WAITING_BOOTLOAD_CMD	0xc0	/* valid 7 6 bit only */
#define MXT_WAITING_FRAME_DATA	0x80	/* valid 7 6 bit only */
#define MXT_FRAME_CRC_CHECK	0x02
#define MXT_FRAME_CRC_FAIL	0x03
#define MXT_FRAME_CRC_PASS	0x04
#define MXT_APP_CRC_FAIL	0x40	/* valid 7 8 bit only */
#define MXT_BOOT_STATUS_MASK	0x3f
#define MXT_BOOT_EXTENDED_ID	(1 << 5)
#define MXT_BOOT_ID_MASK	0x1f

/* Touchscreen absolute values */
#define MXT_MAX_AREA		0xff

#define OBP_INSTANCES(o) ((u16)((o)->instances_minus_one) + 1)
#define OBP_SIZE(o) ((u16)((o)->size_minus_one) + 1)

#define MSLEEP(ms)	usleep_range(ms*1000, ms*1000)

struct mxt_info {
	u8 family_id;
	u8 variant_id;
	u8 version;
	u8 build;
	u8 matrix_xsize;
	u8 matrix_ysize;
	u8 object_num;
};

struct mxt_object {
	u8 type;
	u16 start_address;
	u8 size_minus_one;
	u8 instances_minus_one;
	u8 num_report_ids;
} __packed;

enum mxt_device_state { INIT, APPMODE, BOOTLOADER, FAILED, SHUTDOWN, SUSPEND };

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mxt_early_suspend(struct early_suspend *es);
static void mxt_late_resume(struct early_suspend *es);
#endif
static int mxt_load_fw(struct device *dev, const char *fn);

/* The platform data for the Atmel maXTouch touchscreen driver */
struct mxt_platform_data {
	unsigned long irqflags;
	u8(*read_chg) (void);
	const char *input_name;
};

static struct mxt_platform_data mxt_pdata = {
	.irqflags = IRQF_TRIGGER_LOW | IRQF_TRIGGER_FALLING,
	.input_name = "mxt1664S-touchscreen",
};

/* Each client has this additional data */
struct mxt_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	char phys[64];		/* device physical location */
	const struct mxt_platform_data *pdata;
	enum mxt_device_state state;
	struct mxt_object *object_table;
	u16 mem_size;
	struct mxt_info *info;
	void *raw_info_block;
	int gpio_reset;
	unsigned int irq;
	unsigned int max_x;
	unsigned int max_y;
	struct bin_attribute mem_access_attr;
	bool debug_enabled;
	u8 max_reportid;
	u32 config_crc;
	u32 info_crc;
	u8 bootloader_addr;
	struct t7_config t7_cfg;
	u8 *msg_buf;
	bool t9_update_input;
	u8 last_message_count;
	u8 num_touchids;
	u8 num_stylusids;

	/* Cached parameters from object table */
	u16 T5_address;
	u8 T5_msg_size;
	u8 T6_reportid;
	u16 T6_address;
	u16 T7_address;
	u8 T9_reportid_min;
	u8 T9_reportid_max;
	u8 T15_reportid_min;
	u8 T15_reportid_max;
	u8 T42_reportid_min;
	u8 T42_reportid_max;
	u16 T44_address;
	u8 T48_reportid;
	u8 T63_reportid_min;
	u8 T63_reportid_max;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

/* I2C slave address pairs */
struct mxt_i2c_address_pair {
	u8 bootloader;
	u8 application;
};

static const struct mxt_i2c_address_pair mxt_i2c_addresses[] = {
#ifdef BOOTLOADER_1664_1188
	{ 0x26, 0x4a },
	{ 0x27, 0x4b },
#else
	{ 0x24, 0x4a },
	{ 0x25, 0x4b },
	{ 0x26, 0x4c },
	{ 0x27, 0x4d },
	{ 0x34, 0x5a },
	{ 0x35, 0x5b },
#endif
};

static bool mxt_object_readable(unsigned int type)
{
	switch (type) {
	case MXT_GEN_COMMAND_T6:
	case MXT_GEN_POWER_T7:
	case MXT_GEN_ACQUIRE_T8:
	case MXT_GEN_DATASOURCE_T53:
	case MXT_TOUCH_MULTI_T9:
	case MXT_TOUCH_KEYARRAY_T15:
	case MXT_TOUCH_PROXIMITY_T23:
	case MXT_TOUCH_PROXKEY_T52:
	case MXT_PROCI_GRIPFACE_T20:
	case MXT_PROCG_NOISE_T22:
	case MXT_PROCI_ONETOUCH_T24:
	case MXT_PROCI_TWOTOUCH_T27:
	case MXT_PROCI_GRIP_T40:
	case MXT_PROCI_PALM_T41:
	case MXT_PROCI_TOUCHSUPPRESSION_T42:
	case MXT_PROCI_STYLUS_T47:
	case MXT_PROCG_NOISESUPPRESSION_T48:
	case MXT_SPT_COMMSCONFIG_T18:
	case MXT_SPT_GPIOPWM_T19:
	case MXT_SPT_SELFTEST_T25:
	case MXT_SPT_CTECONFIG_T28:
	case MXT_SPT_USERDATA_T38:
	case MXT_SPT_DIGITIZER_T43:
	case MXT_SPT_CTECONFIG_T46:
		return true;
	default:
		return false;
	}
}

static void mxt_dump_message(struct mxt_data *data, u8 *message)
{
	print_hex_dump(KERN_DEBUG, "MXT MSG:", DUMP_PREFIX_NONE, 16, 1,
		       message, data->T5_msg_size, false);
}

static int mxt_bootloader_read(struct mxt_data *data, u8 *val,
		unsigned int count)
{
	int ret;
	struct i2c_msg msg;

	msg.addr = data->bootloader_addr;
	msg.flags = data->client->flags & I2C_M_TEN;
	msg.flags |= I2C_M_RD;
	msg.len = count;
	msg.buf = val;

	ret = i2c_transfer(data->client->adapter, &msg, 1);

	return (ret == 1) ? 0 : ret;
}

static int mxt_bootloader_write(struct mxt_data *data, const u8 * const val,
	unsigned int count)
{
	int ret;
	struct i2c_msg msg;

	msg.addr = data->bootloader_addr;
	msg.flags = data->client->flags & I2C_M_TEN;
	msg.len = count;
	msg.buf = (u8 *)val;

	ret = i2c_transfer(data->client->adapter, &msg, 1);

	return (ret == 1) ? 0 : ret;
}

static int mxt_get_bootloader_address(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int i;

	for (i = 0; i < ARRAY_SIZE(mxt_i2c_addresses); i++) {
		if (mxt_i2c_addresses[i].application == client->addr) {
			data->bootloader_addr = mxt_i2c_addresses[i].bootloader;

			dev_info(&client->dev, "Bootloader i2c addr: 0x%02x\n",
				data->bootloader_addr);

			return 0;
		}
	}

	dev_err(&client->dev, "Address 0x%02x not found in address table\n",
		client->addr);
	return -EINVAL;
}

static int mxt_probe_bootloader(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	int ret;
	u8 val;
	bool crc_failure;

	ret = mxt_get_bootloader_address(data);
	if (ret)
		return ret;

	ret = mxt_bootloader_read(data, &val, 1);
	if (ret) {
		dev_err(dev, "%s: i2c recv failed\n", __func__);
		return -EIO;
	}

	/* Check app crc fail mode */
	crc_failure = (val & ~MXT_BOOT_STATUS_MASK) == MXT_APP_CRC_FAIL;

	dev_err(dev, "Detected bootloader, status:%02X%s\n",
		val, crc_failure ? ", APP_CRC_FAIL" : "");

	return 0;
}

static int mxt_wait_for_chg(struct mxt_data *data)
{
	int timeout_counter = 0;
	int count = 1E6;

	if (data->pdata->read_chg == NULL) {
		MSLEEP(10);
		return 0;
	}

	while ((timeout_counter++ <= count) && data->pdata->read_chg())
		udelay(20);

	if (timeout_counter > count) {
		dev_err(&data->client->dev, "mxt_wait_for_chg() timeout!\n");
		return -EIO;
	}

	return 0;
}

static u8 mxt_get_bootloader_version(struct mxt_data *data, u8 val)
{
	struct device *dev = &data->client->dev;
	u8 buf[3];

	if (val & MXT_BOOT_EXTENDED_ID) {
		if (mxt_bootloader_read(data, &buf[0], 3) != 0) {
			dev_err(dev, "%s: i2c failure\n", __func__);
			return -EIO;
		}

		dev_info(dev, "Bootloader ID:%d Version:%d\n", buf[1], buf[2]);

		return buf[0];
	} else {
		dev_info(dev, "Bootloader ID:%d\n", val & MXT_BOOT_ID_MASK);

		return val;
	}
}

static int mxt_check_bootloader(struct mxt_data *data,
				unsigned int state)
{
	struct device *dev = &data->client->dev;
	int ret;
	u8 val;

recheck:
	ret = mxt_bootloader_read(data, &val, 1);
	if (ret) {
		dev_err(dev, "%s: i2c recv failed, ret=%d\n",
			__func__, ret);
		return ret;
	}

	if (state == MXT_WAITING_BOOTLOAD_CMD)
		val = mxt_get_bootloader_version(data, val);

	switch (state) {
	case MXT_WAITING_BOOTLOAD_CMD:
		val &= ~MXT_BOOT_STATUS_MASK;
		break;
	case MXT_WAITING_FRAME_DATA:
	case MXT_APP_CRC_FAIL:
		val &= ~MXT_BOOT_STATUS_MASK;
		break;
	case MXT_FRAME_CRC_PASS:
		if (val == MXT_FRAME_CRC_CHECK) {
			mxt_wait_for_chg(data);
			goto recheck;
		} else if (val == MXT_FRAME_CRC_FAIL) {
			dev_err(dev, "Bootloader CRC fail\n");
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	if (val != state) {
		dev_err(dev, "Invalid bootloader mode state 0x%02X\n", val);
		return -EINVAL;
	}

	return 0;
}

static int mxt_send_bootloader_cmd(struct mxt_data *data, bool unlock)
{
	int ret;
	u8 buf[2];

	if (unlock) {
		buf[0] = MXT_UNLOCK_CMD_LSB;
		buf[1] = MXT_UNLOCK_CMD_MSB;
	} else {
		buf[0] = 0x01;
		buf[1] = 0x01;
	}

	ret = mxt_bootloader_write(data, buf, 2);
	if (ret) {
		dev_err(&data->client->dev, "%s: i2c send failed, ret=%d\n",
				__func__, ret);
		return ret;
	}

	return 0;
}

static int __mxt_read_reg(struct i2c_client *client,
			       u16 reg, u16 len, void *val)
{
	struct i2c_msg xfer[2];
	u8 buf[2];
	int ret;
	bool retry = false;

	buf[0] = reg & 0xff;
	buf[1] = (reg >> 8) & 0xff;

	/* Write register */
	xfer[0].addr = client->addr;
	xfer[0].flags = 0;
	xfer[0].len = 2;
	xfer[0].buf = buf;

	/* Read data */
	xfer[1].addr = client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = len;
	xfer[1].buf = val;

retry_read:
	ret = i2c_transfer(client->adapter, xfer, ARRAY_SIZE(xfer));
	if (ret != ARRAY_SIZE(xfer)) {
		if (!retry) {
			dev_dbg(&client->dev, "%s: i2c retry\n", __func__);
			MSLEEP(MXT_WAKEUP_TIME);
			retry = true;
			goto retry_read;
		} else {
			dev_err(&client->dev, "%s: i2c transfer failed (%d)\n",
				__func__, ret);
			return -EIO;
		}
	}

	return 0;
}

static int __mxt_write_reg(struct i2c_client *client, u16 reg, u16 len,
			   const void *val)
{
	u8 *buf;
	size_t count;
	int ret;
	bool retry = false;

	count = len + 2;
	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = reg & 0xff;
	buf[1] = (reg >> 8) & 0xff;
	memcpy(&buf[2], val, len);

retry_write:
	ret = i2c_master_send(client, buf, count);
	if (ret == count) {
		ret = 0;
	} else {
		if (!retry) {
			dev_dbg(&client->dev, "%s: i2c retry\n", __func__);
			MSLEEP(MXT_WAKEUP_TIME);
			retry = true;
			goto retry_write;
		} else {
			dev_err(&client->dev, "%s: i2c send failed (%d)\n",
				__func__, ret);
			ret = -EIO;
		}
	}

	kfree(buf);
	return ret;
}

static int mxt_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	return __mxt_write_reg(client, reg, 1, &val);
}

static struct mxt_object *
mxt_get_object(struct mxt_data *data, u8 type)
{
	struct mxt_object *object;
	int i;

	for (i = 0; i < data->info->object_num; i++) {
		object = data->object_table + i;
		if (object->type == type)
			return object;
	}

	dev_err(&data->client->dev, "Invalid object type T%u\n", type);
	return NULL;
}

static void mxt_proc_t6_messages(struct mxt_data *data, u8 *msg)
{
	struct device *dev = &data->client->dev;
	u8 status = msg[1];
	u32 crc = msg[2] | (msg[3] << 8) | (msg[4] << 16);

	if (crc != data->config_crc) {
		data->config_crc = crc;
		dev_dbg(dev, "T6 Config Checksum: 0x%06X\n", crc);
	}

	if (status)
		dev_dbg(dev, "T6 Status 0x%02X%s%s%s%s%s%s\n",
			status,
			(status & MXT_T6_STATUS_RESET) ? " RESET" : "",
			(status & MXT_T6_STATUS_OFL) ? " OFL" : "",
			(status & MXT_T6_STATUS_SIGERR) ? " SIGERR" : "",
			(status & MXT_T6_STATUS_CAL) ? " CAL" : "",
			(status & MXT_T6_STATUS_CFGERR) ? " CFGERR" : "",
			(status & MXT_T6_STATUS_COMSERR) ? " COMSERR" : "");
}

static void mxt_input_sync(struct input_dev *input_dev)
{
	input_mt_report_pointer_emulation(input_dev, false);
	input_sync(input_dev);
}

static void mxt_proc_t9_message(struct mxt_data *data, u8 *message)
{
	struct device *dev = &data->client->dev;
	struct input_dev *input_dev = data->input_dev;
	int id;
	u8 status;
	int x;
	int y;
	int area;
	int amplitude;
	u8 vector;
	int tool;

	/* do not report events if input device not yet registered */
	if (!input_dev)
		return;

	id = message[0] - data->T9_reportid_min;
	status = message[1];
	x = (message[2] << 4) | ((message[4] >> 4) & 0xf);
	y = (message[3] << 4) | ((message[4] & 0xf));

	if (data->max_x < 1024)
		x >>= 2;
	if (data->max_y < 1024)
		y >>= 2;

	/* A reported size of zero indicates that the reported touch
	 * is a stylus from a linked Stylus T47 object. */
	if (message[5] == 0) {
		area = 1;
		tool = MT_TOOL_PEN;
	} else {
		area = message[5];
		tool = MT_TOOL_FINGER;
	}

	amplitude = message[6];
	vector = message[7];

	dev_dbg(dev,
		"[%u] %c%c%c%c%c%c%c%c x: %5u y: %5u area: %3u amp: %3u vector: %02X\n",
		id,
		(status & MXT_T9_DETECT) ? 'D' : '.',
		(status & MXT_T9_PRESS) ? 'P' : '.',
		(status & MXT_T9_RELEASE) ? 'R' : '.',
		(status & MXT_T9_MOVE) ? 'M' : '.',
		(status & MXT_T9_VECTOR) ? 'V' : '.',
		(status & MXT_T9_AMP) ? 'A' : '.',
		(status & MXT_T9_SUPPRESS) ? 'S' : '.',
		(status & MXT_T9_UNGRIP) ? 'U' : '.',
		x, y, area, amplitude, vector);

	input_mt_slot(input_dev, id);

	if (status & MXT_T9_DETECT) {
		/* Multiple bits may be set if the host is slow to read the
		 * status messages, indicating all the events that have
		 * happened */
		if (status & MXT_T9_RELEASE) {
			input_mt_report_slot_state(input_dev, tool, 0);
			mxt_input_sync(input_dev);
		}

		/* Touch active */
		input_mt_report_slot_state(input_dev, tool, 1);
		input_report_abs(input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(input_dev, ABS_MT_PRESSURE, amplitude);
		input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, area);
		input_report_abs(input_dev, ABS_MT_ORIENTATION, vector);
	} else {
		/* Touch no longer active, close out slot */
		input_mt_report_slot_state(input_dev, tool, 0);
	}

	data->t9_update_input = true;
}

static void mxt_proc_t15_messages(struct mxt_data *data, u8 *msg)
{
	struct input_dev *input_dev = data->input_dev;
	struct device *dev = &data->client->dev;
	u8 key;
	bool curr_state, new_state;
	bool sync = false;
	unsigned long keystates = le32_to_cpu(msg[2]);

	for (key = 0; key < ARRAY_SIZE(mxt_t15_keys); key++) {
		curr_state = test_bit(key, &mxt_t15_keystatus);
		new_state = test_bit(key, &keystates);

		if (!curr_state && new_state) {
			dev_dbg(dev, "T15 key press: %u\n", key);
			__set_bit(key, &mxt_t15_keystatus);
			input_event(input_dev, EV_KEY, mxt_t15_keys[key], 1);
			sync = true;
		} else if (curr_state && !new_state) {
			dev_dbg(dev, "T15 key release: %u\n", key);
			__clear_bit(key, &mxt_t15_keystatus);
			input_event(input_dev, EV_KEY, mxt_t15_keys[key], 0);
			sync = true;
		}
	}

	if (sync)
		input_sync(input_dev);
}

static void mxt_proc_t42_messages(struct mxt_data *data, u8 *msg)
{
	struct device *dev = &data->client->dev;
	u8 status = msg[1];

	if (status & MXT_T42_MSG_TCHSUP)
		dev_info(dev, "T42 suppress\n");
	else
		dev_info(dev, "T42 normal\n");
}

static int mxt_proc_t48_messages(struct mxt_data *data, u8 *msg)
{
	struct device *dev = &data->client->dev;
	u8 status, state;

	status = msg[1];
	state  = msg[4];

	dev_dbg(dev, "T48 state %d status %02X %s%s%s%s%s\n",
			state,
			status,
			(status & 0x01) ? "FREQCHG " : "",
			(status & 0x02) ? "APXCHG " : "",
			(status & 0x04) ? "ALGOERR " : "",
			(status & 0x10) ? "STATCHG " : "",
			(status & 0x20) ? "NLVLCHG " : "");

	return 0;
}

static void mxt_proc_t63_messages(struct mxt_data *data, u8 *msg)
{
	struct device *dev = &data->client->dev;
	struct input_dev *input_dev = data->input_dev;
	u8 id;
	u16 x, y;
	u8 pressure;

	if (!input_dev)
		return;

	/* stylus slots come after touch slots */
	id = data->num_touchids + (msg[0] - data->T63_reportid_min);

	if (id < 0 || id > (data->num_touchids + data->num_stylusids)) {
		dev_err(dev, "invalid stylus id %d, max slot is %d\n",
			id, data->num_stylusids);
		return;
	}

	x = msg[3] | (msg[4] << 8);
	y = msg[5] | (msg[6] << 8);
	pressure = msg[7] & MXT_STYLUS_PRESSURE_MASK;

	dev_dbg(dev,
		"[%d] %c%c%c%c x: %d y: %d pressure: %d stylus:%c%c%c%c\n",
		id,
		(msg[1] & MXT_STYLUS_SUPPRESS) ? 'S' : '.',
		(msg[1] & MXT_STYLUS_MOVE)     ? 'M' : '.',
		(msg[1] & MXT_STYLUS_RELEASE)  ? 'R' : '.',
		(msg[1] & MXT_STYLUS_PRESS)    ? 'P' : '.',
		x, y, pressure,
		(msg[2] & MXT_STYLUS_BARREL) ? 'B' : '.',
		(msg[2] & MXT_STYLUS_ERASER) ? 'E' : '.',
		(msg[2] & MXT_STYLUS_TIP)    ? 'T' : '.',
		(msg[2] & MXT_STYLUS_DETECT) ? 'D' : '.');

	input_mt_slot(input_dev, id);

	if (msg[2] & MXT_STYLUS_DETECT) {
		input_mt_report_slot_state(input_dev, MT_TOOL_PEN, 1);
		input_report_abs(input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(input_dev, ABS_MT_PRESSURE, pressure);
	} else {
		input_mt_report_slot_state(input_dev, MT_TOOL_PEN, 0);
	}

	input_report_key(input_dev, BTN_STYLUS, (msg[2] & MXT_STYLUS_ERASER));
	input_report_key(input_dev, BTN_STYLUS2, (msg[2] & MXT_STYLUS_BARREL));

	mxt_input_sync(input_dev);
}

static int mxt_proc_message(struct mxt_data *data, u8 *message)
{
	u8 report_id = message[0];
	bool handled = false;

	if (data->state != APPMODE)
		return 0;

	if (report_id == MXT_RPTID_NOMSG)
		return 0;

	if (report_id == data->T6_reportid) {
		mxt_proc_t6_messages(data, message);
		handled = true;
	}
	if (report_id >= data->T9_reportid_min
	    && report_id <= data->T9_reportid_max) {
		mxt_proc_t9_message(data, message);
		handled = true;
	} else if (report_id >= data->T63_reportid_min
		   && report_id <= data->T63_reportid_max) {
#if SUPPORT_STYLUS
		mxt_proc_t63_messages(data, message);
#endif
		handled = true;
	} else if (report_id >= data->T42_reportid_min
		   && report_id <= data->T42_reportid_max) {
		mxt_proc_t42_messages(data, message);
		handled = true;
	} else if (report_id == data->T48_reportid) {
		mxt_proc_t48_messages(data, message);
		handled = true;
	} else if (report_id >= data->T15_reportid_min
		   && report_id <= data->T15_reportid_max) {
		mxt_proc_t15_messages(data, message);
	}

	if (!handled || data->debug_enabled)
		mxt_dump_message(data, message);

	return 1;
}

static int mxt_read_and_process_messages(struct mxt_data *data, u8 count)
{
	struct device *dev = &data->client->dev;
	int ret;
	int i;
	u8 num_valid = 0;

	/* Safety check for msg_buf */
	if (count > data->max_reportid)
		return -EINVAL;

	/* Process remaining messages if necessary */
	ret = __mxt_read_reg(data->client, data->T5_address,
				data->T5_msg_size * count, data->msg_buf);
	if (ret) {
		dev_err(dev, "Failed to read %u messages (%d)\n", count, ret);
		return ret;
	}

	for (i = 0;  i < count; i++) {
		ret = mxt_proc_message(data,
			data->msg_buf + data->T5_msg_size * i);

		if (ret == 1)
			num_valid++;
	}

	/* return number of messages read */
	return num_valid;
}

static irqreturn_t mxt_process_messages_t44(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	int ret;
	u8 count, num_left;

	/* Read T44 and T5 together */
	ret = __mxt_read_reg(data->client, data->T44_address,
		data->T5_msg_size + 1, data->msg_buf);
	if (ret) {
		dev_err(dev, "Failed to read T44 and T5 (%d)\n", ret);
		return IRQ_NONE;
	}

	count = data->msg_buf[0];

	if (count == 0) {
		dev_warn(dev, "Interrupt triggered but zero messages\n");
		return IRQ_NONE;
	} else if (count > data->max_reportid) {
		dev_err(dev, "T44 count exceeded max report id\n");
		count = data->max_reportid;
	}

	/* Process first message */
	ret = mxt_proc_message(data, data->msg_buf + 1);
	if (ret < 0) {
		dev_warn(dev, "Unexpected invalid message\n");
		return IRQ_NONE;
	}

	num_left = count - 1;

	/* Process remaining messages if necessary */
	if (num_left) {
		ret = mxt_read_and_process_messages(data, num_left);
		if (ret < 0)
			goto end;
		else if (ret != num_left)
			dev_warn(dev, "Unexpected invalid message\n");
	}

end:
	if (data->t9_update_input) {
		mxt_input_sync(data->input_dev);
		data->t9_update_input = false;
	}

	return IRQ_HANDLED;
}

static int mxt_process_messages_until_invalid(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	int count, read;
	u8 tries = 2;

	count = data->max_reportid;

	/* Read messages until we force an invalid */
	do {
		read = mxt_read_and_process_messages(data, count);
		if (read < count)
			return 0;
	} while (--tries);

	if (data->t9_update_input) {
		mxt_input_sync(data->input_dev);
		data->t9_update_input = false;
	}

	dev_err(dev, "CHG pin isn't cleared\n");
	return -EBUSY;
}

static irqreturn_t mxt_process_messages(struct mxt_data *data)
{
	int total_handled, num_handled;
	u8 count = data->last_message_count;

	if (count < 1 || count > data->max_reportid)
		count = 1;

	/* include final invalid message */
	total_handled = mxt_read_and_process_messages(data, count + 1);
	if (total_handled < 0)
		return IRQ_NONE;
	/* if there were invalid messages, then we are done */
	else if (total_handled <= count)
		goto update_count;

	/* read two at a time until an invalid message or else we reach
	 * reportid limit */
	do {
		num_handled = mxt_read_and_process_messages(data, 2);
		if (num_handled < 0)
			return IRQ_NONE;

		total_handled += num_handled;

		if (num_handled < 2)
			break;
	} while (total_handled < data->num_touchids);

update_count:
	data->last_message_count = total_handled;

	if (data->t9_update_input) {
		mxt_input_sync(data->input_dev);
		data->t9_update_input = false;
	}

	return IRQ_HANDLED;
}

static irqreturn_t mxt_interrupt(int irq, void *dev_id)
{
	struct mxt_data *data = dev_id;

	if (data->T44_address)
		return mxt_process_messages_t44(data);
	else
		return mxt_process_messages(data);
}

static int mxt_t6_command(struct mxt_data *data, u16 cmd_offset,
		u8 value, bool wait)
{
	u16 reg;
	u8 command_register;
	int ret;
	int timeout_counter = 0;

	reg = data->T6_address + cmd_offset;

	ret = mxt_write_reg(data->client, reg, value);
	if (ret)
		return ret;

	if (!wait)
		return 0;

	do {
		MSLEEP(20);
		ret = __mxt_read_reg(data->client, reg, 1, &command_register);
		if (ret)
			return ret;
	} while ((command_register != 0) && (timeout_counter++ <= 100));

	if (timeout_counter > 100) {
		dev_err(&data->client->dev, "Command failed!\n");
		return -EIO;
	}

	return 0;
}

static int mxt_soft_reset(struct mxt_data *data, u8 value)
{
	int ret;
	struct device *dev = &data->client->dev;
	int i;

	dev_info(dev, "Resetting chip\n");
#ifdef MXT_FORCE_BOOTLOADER
	if (MXT_BOOT_VALUE == value) {
		dev_info(dev, "Force to enter bootloader\n");
		gpio_request(data->gpio_reset, "ts_rst");
		gpio_direction_output(data->gpio_reset, 0);
		for (i = 1; i < 10; i++) {
			MSLEEP(1);
			gpio_set_value(data->gpio_reset, 1);
			MSLEEP(88);
			gpio_set_value(data->gpio_reset, 0);
		}
		MSLEEP(1);
		gpio_set_value(data->gpio_reset, 1);
		MSLEEP(100);
		MSLEEP(MXT_RESET_TIME * 3);
		gpio_free(data->gpio_reset);

		return 0;
	}
#endif
	ret = mxt_t6_command(data, MXT_COMMAND_RESET, value, false);
	if (ret)
		return ret;

	if (data->pdata->read_chg == NULL)
		MSLEEP(MXT_RESET_NOCHGREAD);
	else {
		MSLEEP(MXT_RESET_TIME);
		mxt_wait_for_chg(data);
	}

	return 0;
}

static void mxt_read_current_crc(struct mxt_data *data)
{
	/* on failure, CRC is set to 0 and config will always be downloaded */
	data->config_crc = 0;

	mxt_t6_command(data, MXT_COMMAND_REPORTALL, 1, true);

	/* Read all messages until invalid, this will update the config crc
	 * stored in mxt_data. On failure, CRC is set to 0 and config will
	 * always be downloaded */
	mxt_process_messages_until_invalid(data);
}

static void mxt_calc_crc24(u32 *crc, u8 firstbyte, u8 secondbyte)
{
	static const unsigned int crcpoly = 0x80001B;
	u32 result;
	u32 data_word;

	data_word = (secondbyte << 8) | firstbyte;
	result = ((*crc << 1) ^ data_word);

	if (result & 0x1000000)
		result ^= crcpoly;

	*crc = result;
}

static u32 mxt_calculate_crc(u8 *base, off_t start_off, off_t end_off)
{
	u32 crc = 0;
	u8 *ptr = base + start_off;
	u8 *last_val = base + end_off - 1;

	if (end_off < start_off)
		return -EINVAL;

	while (ptr < last_val) {
		mxt_calc_crc24(&crc, *ptr, *(ptr + 1));
		ptr += 2;
	}

	/* if len is odd, fill the last byte with 0 */
	if (ptr == last_val)
		mxt_calc_crc24(&crc, *ptr, 0);

	/* Mask to 24-bit */
	return crc & 0x00FFFFFF;
}

static int mxt_init_t7_power_cfg(struct mxt_data *data);

static int mxt_check_reg_init(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	struct mxt_info cfg_info;
	struct mxt_object *object;
	const struct firmware *cfg = NULL;
	int ret;
	int offset;
	int data_pos;
	int byte_offset;
	int i;
	int config_start_offset;
	u32 info_crc, config_crc, calculated_crc;
	u8 *config_mem;
	size_t config_mem_size;
	unsigned int type, instance, size;
	u8 val;
	u16 reg;

	ret = request_firmware(&cfg, MXT_CFG_NAME, dev);
	if (ret < 0) {
		dev_err(dev, "Failure to request config file %s\n",
			MXT_CFG_NAME);
		return 0;
	}

	mxt_read_current_crc(data);

	if (strncmp(cfg->data, MXT_CFG_MAGIC, strlen(MXT_CFG_MAGIC))) {
		dev_err(dev, "Unrecognised config file\n");
		ret = -EINVAL;
		goto release;
	}

	data_pos = strlen(MXT_CFG_MAGIC);

	/* Load information block and check */
	for (i = 0; i < sizeof(struct mxt_info); i++) {
		ret = sscanf(cfg->data + data_pos, "%hhx%n",
			     (unsigned char *)&cfg_info + i,
			     &offset);
		if (ret != 1) {
			dev_err(dev, "Bad format\n");
			ret = -EINVAL;
			goto release;
		}

		data_pos += offset;
	}

	/* Read CRCs */
	ret = sscanf(cfg->data + data_pos, "%x%n", &info_crc, &offset);
	if (ret != 1) {
		dev_err(dev, "Bad format: failed to parse Info CRC\n");
		ret = -EINVAL;
		goto release;
	}
	data_pos += offset;

	ret = sscanf(cfg->data + data_pos, "%x%n", &config_crc, &offset);
	if (ret != 1) {
		dev_err(dev, "Bad format: failed to parse Config CRC\n");
		ret = -EINVAL;
		goto release;
	}
	data_pos += offset;

	/* The Info Block CRC is calculated over mxt_info and the object table
	 * If it does not match then we are trying to load the configuration
	 * from a different chip or firmware version, so the configuration CRC
	 * is invalid anyway. */
	if (info_crc == data->info_crc) {
		if (config_crc == 0 || data->config_crc == 0) {
			dev_info(dev, "CRC zero, attempting to apply config\n");
		} else if (config_crc == data->config_crc) {
			dev_info(dev, "Config CRC 0x%06X: OK\n",
					data->config_crc);
			ret = 0;
			goto release;
		} else {
			char *buf = kzalloc(cfg->size + 1, GFP_KERNEL);
			if (buf) {
				strncpy(buf, cfg->data, cfg->size);
				dev_err(dev, "cfg->data:%s, data_pos:%d, ret:%d\n",
						buf, data_pos, ret);
				kfree(buf);
			}
			dev_info(dev, "Config CRC 0x%06X: does not match file 0x%06X\n",
				 data->config_crc, config_crc);
		}
	} else {
		dev_warn(dev,
			"Info Block CRC mismatch - device=0x%06X file=0x%06X\n",
			data->info_crc, info_crc);
	}

	/* Malloc memory to store configuration */
	config_start_offset = MXT_OBJECT_START
		+ data->info->object_num * sizeof(struct mxt_object);
	config_mem_size = data->mem_size - config_start_offset;
	config_mem = kzalloc(config_mem_size, GFP_KERNEL);
	if (!config_mem) {
		dev_err(dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto release;
	}

	while (data_pos < cfg->size) {
		/* Read type, instance, length */
		ret = sscanf(cfg->data + data_pos, "%x %x %x%n",
			     &type, &instance, &size, &offset);
		if (ret == 0) {
			/* EOF */
			break;
		} else if (ret != 3) {
			dev_err(dev, "Bad format: failed to parse object\n");
			ret = -EINVAL;
			goto release_mem;
		}
		data_pos += offset;

		object = mxt_get_object(data, type);
		if (!object) {
			ret = -EINVAL;
			goto release_mem;
		}

		if (instance >= OBP_INSTANCES(object)) {
			dev_err(dev, "Object instances exceeded!\n");
			ret = -EINVAL;
			goto release_mem;
		}

		reg = object->start_address + OBP_SIZE(object) * instance;

		if (size > OBP_SIZE(object)) {
			/* Either we are in fallback mode due to wrong
			 * config or config from a later fw version,
			 * or the file is corrupt or hand-edited */
			dev_warn(dev, "Discarding %u bytes in T%u!\n",
				 size - OBP_SIZE(object), type);

			size = OBP_SIZE(object);
		} else if (OBP_SIZE(object) > size) {
			/* If firmware is upgraded, new bytes may be added to
			 * end of objects. It is generally forward compatible
			 * to zero these bytes - previous behaviour will be
			 * retained. However this does invalidate the CRC and
			 * will force fallback mode until the configuration is
			 * updated. We warn here but do nothing else - the
			 * malloc has zeroed the entire configuration. */
			dev_warn(dev, "Zeroing %d byte(s) in T%d\n",
				 OBP_SIZE(object) - size, type);
		}

		for (i = 0; i < size; i++) {
			ret = sscanf(cfg->data + data_pos, "%hhx%n",
				     &val,
				     &offset);
			if (ret != 1) {
				dev_err(dev, "Bad format in T%d\n", type);
				ret = -EINVAL;
				goto release_mem;
			}

			byte_offset = reg + i - config_start_offset;

			if ((byte_offset >= 0)
			    && (byte_offset <= config_mem_size)) {
				*(config_mem + byte_offset) = val;
			} else {
				dev_err(dev, "Bad object: reg:%d, T%d, ofs=%d\n",
					reg, object->type, byte_offset);
				ret = -EINVAL;
				goto release_mem;
			}

			data_pos += offset;
		}

	}

	/* calculate crc of the received configs (not the raw config file) */
	if (data->T7_address < config_start_offset) {
		dev_err(dev, "Bad T7 address, T7addr = %x, config offset %x\n",
				data->T7_address, config_start_offset);
		ret = 0;
		goto release_mem;
	}

	calculated_crc = mxt_calculate_crc(config_mem,
		data->T7_address - config_start_offset, config_mem_size);

	/* check the crc, calculated should same as what's in file */
	if (config_crc > 0 && (config_crc != calculated_crc)) {
		dev_err(dev, "CRC mismatch in config file, calculated=%06X, file=%06X\n",
				calculated_crc, config_crc);
		ret = 0;
		goto release_mem;
	}

	/* Write configuration as blocks */
	byte_offset = 0;
	while (byte_offset < config_mem_size) {
		size = config_mem_size - byte_offset;

		if (size > MXT_MAX_BLOCK_WRITE)
			size = MXT_MAX_BLOCK_WRITE;

		ret = __mxt_write_reg(data->client,
				      config_start_offset + byte_offset,
				      size, config_mem + byte_offset);
		if (ret != 0) {
			dev_err(dev, "Config write error, ret=%d\n", ret);
			goto release_mem;
		}

		byte_offset += size;
	}

	ret = mxt_t6_command(data, MXT_COMMAND_BACKUPNV,
			MXT_BACKUP_VALUE, false);
	if (ret)
		goto release_mem;

	ret = mxt_soft_reset(data, MXT_RESET_VALUE);
	if (ret)
		goto release_mem;

	dev_info(dev, "Config written\n");

	/* T7 config may have changed */
	mxt_init_t7_power_cfg(data);

release_mem:
	kfree(config_mem);
release:
	if (ret) {
		char *buf = kzalloc(cfg->size+1, GFP_KERNEL);
		if (buf) {
			strncpy(buf, cfg->data, cfg->size);
			dev_err(dev, "cfg->data:%s, data_pos:%d, ret:%d\n",
					buf, data_pos, ret);
			kfree(buf);
		}
	}
	release_firmware(cfg);
	return ret;
}

static int mxt_set_t7_power_cfg(struct mxt_data *data, u8 sleep)
{
	struct device *dev = &data->client->dev;
	int error;
	struct t7_config *new_config;
	struct t7_config deepsleep = { .active = 0, .idle = 0 };

	if (sleep == MXT_POWER_CFG_DEEPSLEEP)
		new_config = &deepsleep;
	else
		new_config = &data->t7_cfg;

	error = __mxt_write_reg(data->client, data->T7_address,
			sizeof(data->t7_cfg),
			new_config);
	if (error)
		return error;

	dev_dbg(dev, "Set T7 ACTV:%d IDLE:%d\n",
		new_config->active, new_config->idle);

	return 0;
}

static int mxt_init_t7_power_cfg(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	int error;
	bool retry = false;

recheck:
	error = __mxt_read_reg(data->client, data->T7_address,
				sizeof(data->t7_cfg), &data->t7_cfg);
	if (error)
		return error;

	if (data->t7_cfg.active == 0 || data->t7_cfg.idle == 0) {
		if (!retry) {
			dev_info(dev, "T7 cfg zero, resetting\n");
			error = mxt_soft_reset(data, MXT_RESET_VALUE);
			if (error)
				return error;

			retry = true;
			goto recheck;
		} else {
		    dev_dbg(dev, "T7 cfg zero after reset, overriding\n");
		    data->t7_cfg.active = 20;
		    data->t7_cfg.idle = 100;
		    return mxt_set_t7_power_cfg(data, MXT_POWER_CFG_RUN);
		}
	} else {
		dev_info(dev, "Initialised power cfg: ACTV %d, IDLE %d\n",
				data->t7_cfg.active, data->t7_cfg.idle);
		return 0;
	}
}

static int mxt_parse_object_table(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int i;
	u8 reportid;
	u16 end_address;

	dev_dbg(&client->dev, "Object num: %d\n", data->info->object_num);

	/* Valid Report IDs start counting from 1 */
	reportid = 1;
	data->mem_size = 0;
	for (i = 0; i < data->info->object_num; i++) {
		struct mxt_object *object = data->object_table + i;
		u8 min_id, max_id;

		le16_to_cpus(&object->start_address);

		if (object->num_report_ids) {
			min_id = reportid;
			reportid += object->num_report_ids *
					OBP_INSTANCES(object);
			max_id = reportid - 1;
		} else {
			min_id = 0;
			max_id = 0;
		}

		dev_dbg(&data->client->dev,
			"T%u Start:%u Size:%u Instances:%u Report IDs:%u-%u\n",
			object->type, object->start_address, OBP_SIZE(object),
			OBP_INSTANCES(object), min_id, max_id);

		switch (object->type) {
		case MXT_GEN_MESSAGE_T5:
			if (data->info->family_id == 0x80) {
				/* On mXT224 read and discard unused CRC byte
				 * otherwise DMA reads are misaligned */
				data->T5_msg_size = OBP_SIZE(object);
			} else {
				/* CRC not enabled,
				   therefore don't read last byte */
				data->T5_msg_size = OBP_SIZE(object) - 1;
			}
			data->T5_address = object->start_address;
		case MXT_GEN_COMMAND_T6:
			data->T6_reportid = min_id;
			data->T6_address = object->start_address;
			break;
		case MXT_GEN_POWER_T7:
			data->T7_address = object->start_address;
			break;
		case MXT_TOUCH_MULTI_T9:
			data->T9_reportid_min = min_id;
			data->T9_reportid_max = max_id;
			data->num_touchids =
				object->num_report_ids * OBP_INSTANCES(object);
			break;
		case MXT_TOUCH_KEYARRAY_T15:
			data->T15_reportid_min = min_id;
			data->T15_reportid_max = max_id;
			break;
		case MXT_PROCI_TOUCHSUPPRESSION_T42:
			data->T42_reportid_min = min_id;
			data->T42_reportid_max = max_id;
			break;
		case MXT_SPT_MESSAGECOUNT_T44:
			data->T44_address = object->start_address;
			break;
		case MXT_SPT_NOISESUPPRESSION_T48:
			data->T48_reportid = min_id;
			break;
		case MXT_PROCI_ACTIVE_STYLUS_T63:
			data->T63_reportid_min = min_id;
			data->T63_reportid_max = max_id;
			data->num_stylusids =
				object->num_report_ids * OBP_INSTANCES(object);
			break;
		}

		end_address = object->start_address
			+ OBP_SIZE(object) * OBP_INSTANCES(object) - 1;

		if (end_address >= data->mem_size)
			data->mem_size = end_address + 1;
	}

	/* Store maximum reportid */
	data->max_reportid = reportid;

	/* If T44 exists, T9 position has to be directly after */
	if (data->T44_address && (data->T5_address != data->T44_address + 1)) {
		dev_err(&client->dev, "Invalid T44 position\n");
		return -EINVAL;
	}

	data->msg_buf = kcalloc(data->max_reportid, data->T5_msg_size,
				GFP_KERNEL);
	if (!data->msg_buf) {
		dev_err(&client->dev, "Failed to allocate message buffer\n");
		return -ENOMEM;
	}

	return 0;
}

static void mxt_free_object_table(struct mxt_data *data)
{
	kfree(data->raw_info_block);
	data->raw_info_block = NULL;
	data->info = NULL;
	data->object_table = NULL;
	kfree(data->msg_buf);
	data->msg_buf = NULL;
	data->T5_address = 0;
	data->T5_msg_size = 0;
	data->T6_address = 0;
	data->T6_reportid = 0;
	data->T7_address = 0;
	data->T9_reportid_min = 0;
	data->T9_reportid_max = 0;
}

static int mxt_read_info_block(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int error;
	size_t size;
	void *buf;
	struct mxt_info *info;
	u32 calculated_crc;
	u8 *crc_ptr;

	/* If info block already allocated, free it */
	if (data->raw_info_block != NULL)
		mxt_free_object_table(data);

	/* Read 7-byte ID information block starting at address 0 */
	size = sizeof(struct mxt_info);
	buf = kzalloc(size, GFP_KERNEL);
	if (!buf) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	error = __mxt_read_reg(client, 0, size, buf);
	if (error)
		goto err_free_mem;

	/* Resize buffer to give space for rest of info block */
	info = (struct mxt_info *)buf;
	size += (info->object_num * sizeof(struct mxt_object))
		+ MXT_INFO_CHECKSUM_SIZE;

	buf = krealloc(buf, size, GFP_KERNEL);
	if (!buf) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		error = -ENOMEM;
		goto err_free_mem;
	}

	/* Read rest of info block */
	error = __mxt_read_reg(client, MXT_OBJECT_START,
			       size - MXT_OBJECT_START,
			       buf + MXT_OBJECT_START);
	if (error)
		goto err_free_mem;

	/* Extract & calculate checksum */
	crc_ptr = buf + size - MXT_INFO_CHECKSUM_SIZE;
	data->info_crc = crc_ptr[0] | (crc_ptr[1] << 8) | (crc_ptr[2] << 16);

	calculated_crc = mxt_calculate_crc(buf, 0,
			size - MXT_INFO_CHECKSUM_SIZE);

	/* CRC mismatch can be caused by data corruption due to I2C comms
	 * issue or else device is not using Object Based Protocol */
	if (data->info_crc != calculated_crc) {
		dev_err(&client->dev,
			"Info Block CRC error calculated=0x%06X read=0x%06X\n",
			data->info_crc, calculated_crc);
		return -EIO;
	}

	/* Save pointers in device data structure */
	data->raw_info_block = buf;
	data->info = (struct mxt_info *)buf;
	data->object_table = (struct mxt_object *)(buf + MXT_OBJECT_START);

	dev_info(&client->dev,
			"Family ID: %u Variant ID: %u Firmware: V%u.%u.%02X",
			data->info->family_id, data->info->variant_id,
			data->info->version >> 4, data->info->version & 0xf,
			data->info->build);

	/* Parse object table information */
	error = mxt_parse_object_table(data);
	if (error) {
		dev_err(&client->dev, "Error %d reading object table\n", error);
		mxt_free_object_table(data);
		return error;
	}

	return 0;

err_free_mem:
	kfree(buf);
	return error;
}

static int mxt_read_t9_resolution(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int error;
	struct t9_range range;
	unsigned char orient;
	struct mxt_object *object;

	object = mxt_get_object(data, MXT_TOUCH_MULTI_T9);
	if (!object)
		return -EINVAL;

	error = __mxt_read_reg(client,
			       object->start_address + MXT_T9_RANGE,
			       sizeof(range), &range);
	if (error)
		return error;

	le16_to_cpus(range.x);
	le16_to_cpus(range.y);

	error =  __mxt_read_reg(client,
				object->start_address + MXT_T9_ORIENT,
				1, &orient);
	if (error)
		return error;

	/* Handle default values */
	if (range.x == 0)
		range.x = 1023;

	if (range.y == 0)
		range.y = 1023;

	if (orient & MXT_T9_ORIENT_SWITCH) {
		data->max_x = range.y;
		data->max_y = range.x;
	} else {
		data->max_x = range.x;
		data->max_y = range.y;
	}

	dev_info(&client->dev, "Touchscreen size X%uY%u\n",
			data->max_x, data->max_y);

	return 0;
}

static int mxt_initialize(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int error;
	u8 retry_count = 0;
	struct mxt_info info;

	error = __mxt_read_reg(client, 0, sizeof(struct mxt_info), &info);
	if (error || info.family_id != MXT1664S_FAMILY_ID) {
		error = mxt_load_fw(&client->dev, MXT_FW_NAME);
		if (!error) {
			dev_info(&client->dev, "Firmware update succeeded\n");
			MSLEEP(MXT_FWRESET_TIME);
		}
	}

retry_probe:
	error = mxt_read_info_block(data);
	if (error) {
		error = mxt_probe_bootloader(data);
		if (error) {
			/* Chip is not in appmode or bootloader mode */
			return error;
		} else {
			if (++retry_count > 10) {
				dev_err(&client->dev,
					"Could not recover device from "
					"bootloader mode\n");
				data->state = BOOTLOADER;
				/* this is not an error state, we can reflash
				 * from here */
				return 0;
			}

			/* Attempt to exit bootloader into app mode */
			mxt_send_bootloader_cmd(data, false);
			MSLEEP(MXT_FWRESET_TIME);
			goto retry_probe;
		}
	}

	data->state = APPMODE;

	error = mxt_init_t7_power_cfg(data);
	if (error) {
		dev_err(&client->dev, "Failed to initialize power cfg\n");
		return error;
	}

	/* Check register init values */
	error = mxt_check_reg_init(data);
	if (error) {
		dev_err(&client->dev, "Error %d initialising configuration\n",
			error);
		return error;
	}

	return 0;
}

/* Firmware Version is returned as Major.Minor.Build */
static ssize_t mxt_fw_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%u.%u.%02X\n",
			 data->info->version >> 4, data->info->version & 0xf,
			 data->info->build);
}

/* Hardware Version is returned as FamilyID.VariantID */
static ssize_t mxt_hw_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "%u.%u\n",
			data->info->family_id, data->info->variant_id);
}

static ssize_t mxt_show_instance(char *buf, int count,
				 struct mxt_object *object, int instance,
				 const u8 *val)
{
	int i;

	if (OBP_INSTANCES(object) > 1)
		count += scnprintf(buf + count, PAGE_SIZE - count,
				   "Instance %u\n", instance);

	for (i = 0; i < OBP_SIZE(object); i++)
		count += scnprintf(buf + count, PAGE_SIZE - count,
				"\t[%2u]: %02x (%d)\n", i, val[i], val[i]);
	count += scnprintf(buf + count, PAGE_SIZE - count, "\n");

	return count;
}

static ssize_t mxt_object_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	struct mxt_object *object;
	int count = 0;
	int i, j;
	int error;
	u8 *obuf;

	/* Pre-allocate buffer large enough to hold max sized object. */
	obuf = kmalloc(256, GFP_KERNEL);
	if (!obuf)
		return -ENOMEM;

	error = 0;
	for (i = 0; i < data->info->object_num; i++) {
		object = data->object_table + i;

		if (!mxt_object_readable(object->type))
			continue;

		count += scnprintf(buf + count, PAGE_SIZE - count,
				"T%u:\n", object->type);

		for (j = 0; j < OBP_INSTANCES(object); j++) {
			u16 size = OBP_SIZE(object);
			u16 addr = object->start_address + j * size;

			error = __mxt_read_reg(data->client, addr, size, obuf);
			if (error)
				goto done;

			count = mxt_show_instance(buf, count, object, j, obuf);
		}
	}

done:
	kfree(obuf);
	return error ?: count;
}

static int mxt_check_firmware_format(struct device *dev,
		const struct firmware *fw)
{
	unsigned int pos = 0;
	char c;

	while (pos < fw->size) {
		c = *(fw->data + pos);

		if (c < '0' || (c > '9' && c < 'A') || c > 'F')
			return 0;

		pos++;
	}

	/* To convert file try
	 * xxd -r -p mXTXXX__APP_VX-X-XX.enc > maxtouch.fw */
	dev_err(dev, "Aborting: firmware file must be in binary format\n");

	return -1;
}

static int mxt_load_fw(struct device *dev, const char *fn)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	const struct firmware *fw = NULL;
	unsigned int frame_size;
	unsigned int pos = 0;
	unsigned int retry = 0;
	unsigned int frame = 0;
	int ret;

	ret = request_firmware(&fw, fn, dev);
	if (ret < 0) {
		dev_err(dev, "Unable to open firmware %s\n", fn);
		return ret;
	}

	/* Check for incorrect enc file */
	ret = mxt_check_firmware_format(dev, fw);
	if (ret)
		goto release_firmware;

	if (data->state != BOOTLOADER) {
		/* Change to the bootloader mode */
		ret = mxt_soft_reset(data, MXT_BOOT_VALUE);
		if (ret)
			goto release_firmware;

		ret = mxt_get_bootloader_address(data);
		if (ret)
			goto release_firmware;

		data->state = BOOTLOADER;
	}

	ret = mxt_check_bootloader(data, MXT_WAITING_BOOTLOAD_CMD);
	if (ret) {
		mxt_wait_for_chg(data);
		/* Bootloader may still be unlocked from previous update
		 * attempt */
		ret = mxt_check_bootloader(data, MXT_WAITING_FRAME_DATA);
		if (ret) {
			data->state = FAILED;
			goto release_firmware;
		}
	} else {
		dev_info(dev, "Unlocking bootloader\n");

		/* Unlock bootloader */
		ret = mxt_send_bootloader_cmd(data, true);
		if (ret) {
			data->state = FAILED;
			goto release_firmware;
		}
	}

	while (pos < fw->size) {
		mxt_wait_for_chg(data);
		ret = mxt_check_bootloader(data, MXT_WAITING_FRAME_DATA);
		if (ret) {
			data->state = FAILED;
			goto release_firmware;
		}

		frame_size = ((*(fw->data + pos) << 8) | *(fw->data + pos + 1));

		/* Take account of CRC bytes */
		frame_size += 2;

		/* Write one frame to device */
		ret = mxt_bootloader_write(data, fw->data + pos, frame_size);
		if (ret) {
			data->state = FAILED;
			goto release_firmware;
		}

		mxt_wait_for_chg(data);
		ret = mxt_check_bootloader(data, MXT_FRAME_CRC_PASS);
		if (ret) {
			retry++;

			/* Back off by 20ms per retry */
			MSLEEP(retry * 20);

			if (retry > 20) {
				data->state = FAILED;
				goto release_firmware;
			}
		} else {
			retry = 0;
			pos += frame_size;
			frame++;
		}

		if (frame % 10 == 0)
			dev_info(dev, "Updated %d frames, %d/%zd bytes\n",
				 frame, pos, fw->size);
	}

	dev_info(dev, "Finished, sent %d frames, %zd bytes\n", frame, pos);

	data->state = INIT;

release_firmware:
	release_firmware(fw);
	return ret;
}

static ssize_t mxt_update_fw_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int error;

	disable_irq(data->irq);

	error = mxt_load_fw(dev, MXT_FW_NAME);
	if (error) {
		dev_err(dev, "The firmware update failed(%d)\n", error);
		count = error;
	} else {
		dev_info(dev, "The firmware update succeeded\n");

		/* Wait for reset */
		MSLEEP(MXT_FWRESET_TIME);

		mxt_free_object_table(data);

		mxt_initialize(data);
	}

	enable_irq(data->irq);

	return count;
}

static ssize_t mxt_debug_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int count;
	char c;

	c = data->debug_enabled ? '1' : '0';
	count = sprintf(buf, "%c\n", c);

	return count;
}

static ssize_t mxt_debug_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	int i;

	if (sscanf(buf, "%u", &i) == 1 && i < 2) {
		data->debug_enabled = (i == 1);

		dev_dbg(dev, "%s\n", i ? "debug enabled" : "debug disabled");
		return count;
	} else {
		dev_dbg(dev, "debug_enabled write error\n");
		return -EINVAL;
	}
}

static int mxt_check_mem_access_params(struct mxt_data *data, loff_t off,
				       size_t *count)
{
	if (data->state != APPMODE) {
		dev_err(&data->client->dev, "Not in APPMODE\n");
		return -EINVAL;
	}

	if (off >= data->mem_size)
		return -EIO;

	if (off + *count > data->mem_size)
		*count = data->mem_size - off;

	if (*count > MXT_MAX_BLOCK_WRITE)
		*count = MXT_MAX_BLOCK_WRITE;

	return 0;
}

static ssize_t mxt_mem_access_read(struct file *filp, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret = 0;

	ret = mxt_check_mem_access_params(data, off, &count);
	if (ret < 0)
		return ret;

	if (count > 0)
		ret = __mxt_read_reg(data->client, off, count, buf);

	return ret == 0 ? count : ret;
}

static ssize_t mxt_mem_access_write(struct file *filp, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t off,
	size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret = 0;

	ret = mxt_check_mem_access_params(data, off, &count);
	if (ret < 0)
		return ret;

	if (count > 0)
		ret = __mxt_write_reg(data->client, off, count, buf);

	return ret == 0 ? count : 0;
}

static DEVICE_ATTR(fw_version, S_IRUGO, mxt_fw_version_show, NULL);
static DEVICE_ATTR(hw_version, S_IRUGO, mxt_hw_version_show, NULL);
static DEVICE_ATTR(object, S_IRUGO, mxt_object_show, NULL);
static DEVICE_ATTR(update_fw, S_IWUSR, NULL, mxt_update_fw_store);
static DEVICE_ATTR(debug_enable, S_IWUSR | S_IRUSR, mxt_debug_enable_show,
		   mxt_debug_enable_store);

static struct attribute *mxt_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_hw_version.attr,
	&dev_attr_object.attr,
	&dev_attr_update_fw.attr,
	&dev_attr_debug_enable.attr,
	NULL
};

static const struct attribute_group mxt_attr_group = {
	.attrs = mxt_attrs,
};

static void mxt_reset_slots(struct mxt_data *data)
{
	struct input_dev *input_dev = data->input_dev;
	unsigned int num_mt_slots;
	int id;

	num_mt_slots = data->num_touchids + data->num_stylusids;

	for (id = 0; id < num_mt_slots; id++) {
		input_mt_slot(input_dev, id);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 0);
	}

	mxt_input_sync(input_dev);
}

static void mxt_start(struct mxt_data *data)
{
	/* Discard any messages still in message buffer from before chip went
	 * to sleep */
	mxt_process_messages_until_invalid(data);

	mxt_set_t7_power_cfg(data, MXT_POWER_CFG_RUN);

	if (data->state != APPMODE) {
		enable_irq(data->irq);
		data->state = APPMODE;
	}

	/* Recalibrate since chip has been in deep sleep */
	mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
}

static void mxt_stop(struct mxt_data *data)
{
	mxt_set_t7_power_cfg(data, MXT_POWER_CFG_DEEPSLEEP);

	disable_irq(data->irq);

	data->state = SUSPEND;

	mxt_reset_slots(data);
}

static int mxt_input_open(struct input_dev *dev)
{
	struct mxt_data *data = input_get_drvdata(dev);

	mxt_start(data);

	return 0;
}

static void mxt_input_close(struct input_dev *dev)
{
	struct mxt_data *data = input_get_drvdata(dev);

	mxt_stop(data);
}

static int __devinit mxt_initialize_t9_input_device(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	struct input_dev *input_dev;
	int error;
	unsigned int num_mt_slots;
	int key;

	error = mxt_read_t9_resolution(data);
	if (error)
		dev_warn(&client->dev, "Failed to initialize T9 resolution\n");

	input_dev = input_allocate_device();
	if (!data || !input_dev) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	if (data->pdata->input_name)
		input_dev->name = data->pdata->input_name;
	else
		input_dev->name = "Atmel maXTouch Touchscreen";

	snprintf(data->phys, sizeof(data->phys), "i2c-%u-%04x/input0",
		 client->adapter->nr, client->addr);
	input_dev->phys = data->phys;

	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;
	input_dev->open = mxt_input_open;
	input_dev->close = mxt_input_close;

	set_bit(EV_ABS, input_dev->evbit);
	input_set_capability(input_dev, EV_KEY, BTN_TOUCH);

	/* For single touch */
	input_set_abs_params(input_dev, ABS_X,
			     0, data->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_Y,
			     0, data->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE,
			     0, 255, 0, 0);

	/* For multi touch */
	num_mt_slots = data->num_touchids + data->num_stylusids;
	error = input_mt_init_slots(input_dev, num_mt_slots);
	if (error)
		goto err_free_mem;

	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR,
			     0, MXT_MAX_AREA, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
			     0, data->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
			     0, data->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE,
			     0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_ORIENTATION,
			     0, 255, 0, 0);

#if SUPPORT_STYLUS
	/* For T63 active stylus */
	if (data->T63_reportid_min) {
		input_set_capability(input_dev, EV_KEY, BTN_STYLUS);
		input_set_capability(input_dev, EV_KEY, BTN_STYLUS2);
		input_set_abs_params(input_dev, ABS_MT_TOOL_TYPE,
			0, MT_TOOL_MAX, 0, 0);
	}
#endif

	/* For T15 key array */
	if (data->T15_reportid_min) {
		mxt_t15_keystatus = 0;
		for (key = 0; key < ARRAY_SIZE(mxt_t15_keys); key++)
			input_set_capability(input_dev, EV_KEY,
					mxt_t15_keys[key]);
	}

	input_set_drvdata(input_dev, data);

	error = input_register_device(input_dev);
	if (error) {
		dev_err(&client->dev, "Error %d registering input device\n",
			error);
		goto err_free_mem;
	}

	data->input_dev = input_dev;

	return 0;

err_free_mem:
	input_free_device(input_dev);
	return error;
}

static int polling_thread(void *p)
{
	while (1) {
		MSLEEP(100);
		mxt_interrupt(0, p);
	}
}

static int __devinit mxt_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	const struct mxt_platform_data *pdata = &mxt_pdata;
	struct mxt_data *data;
	int error;
	struct acpi_gpio_info gpio_info;

	data = kzalloc(sizeof(struct mxt_data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	data->state = INIT;
	data->client = client;
	data->pdata = pdata;
	data->irq = client->irq;
	data->gpio_reset = acpi_get_gpio_by_index(&client->dev, 0, &gpio_info);
	i2c_set_clientdata(client, data);

	error = sysfs_create_group(&client->dev.kobj, &mxt_attr_group);
	if (error) {
		dev_err(&client->dev, "Failure %d creating sysfs group\n",
			error);
		goto err_free_mem;
	}

	error = mxt_initialize(data);
	if (error)
		goto err_remove_sysfs_group;

	error = mxt_initialize_t9_input_device(data);
	if (error) {
		dev_err(&client->dev, "Error %d registering input device\n",
			error);
		goto err_free_object;
	}

	error = request_threaded_irq(client->irq, NULL, mxt_interrupt,
			pdata->irqflags | IRQF_ONESHOT,
			client->name, data);
	if (error) {
		dev_err(&client->dev, "Failed to register interrupt\n");
		goto err_unregister_device;
	}

	sysfs_bin_attr_init(&data->mem_access_attr);
	data->mem_access_attr.attr.name = "mem_access";
	data->mem_access_attr.attr.mode = S_IRUGO | S_IWUSR;
	data->mem_access_attr.read = mxt_mem_access_read;
	data->mem_access_attr.write = mxt_mem_access_write;
	data->mem_access_attr.size = data->mem_size;

	if (sysfs_create_bin_file(&client->dev.kobj,
				  &data->mem_access_attr) < 0) {
		dev_err(&client->dev, "Failed to create %s\n",
			data->mem_access_attr.attr.name);
		goto err_free_irq;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	data->early_suspend.suspend = mxt_early_suspend;
	data->early_suspend.resume = mxt_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

	return 0;

err_free_irq:
	free_irq(client->irq, data);
err_unregister_device:
	input_unregister_device(data->input_dev);
	data->input_dev = NULL;
err_free_object:
	mxt_free_object_table(data);
err_remove_sysfs_group:
	sysfs_remove_group(&client->dev.kobj, &mxt_attr_group);
err_free_mem:
	kfree(data);
	return error;
}

static int __devexit mxt_remove(struct i2c_client *client)
{
	struct mxt_data *data = i2c_get_clientdata(client);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif
	sysfs_remove_bin_file(&client->dev.kobj, &data->mem_access_attr);
	sysfs_remove_group(&client->dev.kobj, &mxt_attr_group);
	free_irq(data->irq, data);
	input_unregister_device(data->input_dev);
	mxt_free_object_table(data);
	kfree(data);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
atomic_t mxt_early_suspend_flag = ATOMIC_INIT(0);
static int mxt_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mxt_data *data = i2c_get_clientdata(client);
	struct input_dev *input_dev = data->input_dev;
	int temp;
	mutex_lock(&input_dev->mutex);

	temp = atomic_read(&mxt_early_suspend_flag);
	atomic_inc(&mxt_early_suspend_flag);
	if ((input_dev->users) && (!temp))
		mxt_stop(data);

	mutex_unlock(&input_dev->mutex);

	return 0;
}

static int mxt_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mxt_data *data = i2c_get_clientdata(client);
	struct input_dev *input_dev = data->input_dev;
	int temp;
	mutex_lock(&input_dev->mutex);

	atomic_dec(&mxt_early_suspend_flag);
	temp = atomic_read(&mxt_early_suspend_flag);
	if ((input_dev->users) && (!temp))
		mxt_start(data);

	mutex_unlock(&input_dev->mutex);

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mxt_early_suspend(struct early_suspend *es)
{
	struct mxt_data *mxt;
	mxt = container_of(es, struct mxt_data, early_suspend);

	if (mxt_suspend(&mxt->client->dev) != 0)
		dev_err(&mxt->client->dev, "%s: failed\n", __func__);
}

static void mxt_late_resume(struct early_suspend *es)
{
	struct mxt_data *mxt;
	mxt = container_of(es, struct mxt_data, early_suspend);

	if (mxt_resume(&mxt->client->dev) != 0)
		dev_err(&mxt->client->dev, "%s: failed\n", __func__);
}
#endif

#endif

static SIMPLE_DEV_PM_OPS(mxt_pm_ops, mxt_suspend, mxt_resume);

static void mxt_shutdown(struct i2c_client *client)
{
	struct mxt_data *data = i2c_get_clientdata(client);

	disable_irq(data->irq);
	data->state = SHUTDOWN;
}

static const struct i2c_device_id mxt_id[] = {
	{ "atmel_mxt_mxt1664S", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mxt_id);

static struct acpi_device_id mxt_acpi_match[] = {
	{ "ATML1000", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, mxt_acpi_match);

static struct i2c_driver mxt_driver = {
	.driver = {
		.name	= "atmel_mxt_mxt1664S",
		.owner	= THIS_MODULE,
		.acpi_match_table = ACPI_PTR(mxt_acpi_match),
		.pm	= &mxt_pm_ops,
	},
	.probe		= mxt_probe,
	.remove		= __devexit_p(mxt_remove),
	.shutdown	= mxt_shutdown,
	.id_table	= mxt_id,
};
module_i2c_driver(mxt_driver);

/* Module information */
MODULE_AUTHOR("Joonyoung Shim <jy0922.shim@samsung.com>");
MODULE_DESCRIPTION("Atmel maXTouch Touchscreen driver");
MODULE_LICENSE("GPL");

