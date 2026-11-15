// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define 	FMT_CODE (MEDIA_BUS_FMT_UYVY8_1X16)
#define		CAM_STREAM_FORMAT (0x1e)

/* regulator supplies */
static const char * const ov5640_supply_name[] = {
	"DOVDD", /* Digital I/O (1.8V) supply */
	"AVDD",  /* Analog (2.8V) supply */
	"DVDD",  /* Digital Core (1.5V) supply */
};

#define OV5640_NUM_SUPPLIES ARRAY_SIZE(ov5640_supply_name)

struct reg_value {
	u16 reg_addr;
	u8 val;
	u8 mask;
	u32 delay_ms;
};

struct ov5640_dev {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad[2];
	struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */
	struct clk *xclk; /* system clock to OV5640 */
	u32 xclk_freq;

	struct regulator_bulk_data supplies[OV5640_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	bool   upside_down;

	/* lock to protect all members below */
	struct mutex lock;
	bool streaming;
	u8 link_lock_status;
};

static inline struct ov5640_dev *to_ov5640_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov5640_dev, sd);
}

static struct v4l2_mbus_framefmt ov5640_default_fmt = {
	.code = FMT_CODE,
	.width = 1920,
	.height = 1080,
	.colorspace = V4L2_COLORSPACE_REC709,
	.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_REC709),
	.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(V4L2_COLORSPACE_REC709),
	.field = V4L2_FIELD_NONE,
};

static const struct reg_value ov5640_init_setting[] = {    
	{0x3103, 0x11, 0, 0}, //select CLOCK from PAD
	{0x3008, 0x82, 0, 10}, //SW RESET
	{0x3008, 0x42, 0, 0}, //SW Power Down
	{0x3103, 0x03, 0, 0},
	{0x3017, 0x00, 0, 0},
	{0x3018, 0x00, 0, 0},
	{0x3034, 0x18, 0, 0},
	{0x3035, 0x11, 0, 0},
	{0x3036, 0x54, 0, 0},
	{0x3037, 0x13, 0, 0},
	{0x3108, 0x01, 0, 0},
	{0x3630, 0x36, 0, 0},
	{0x3631, 0x0e, 0, 0},
	{0x3632, 0xe2, 0, 0},
	{0x3633, 0x12, 0, 0},
	{0x3621, 0xe0, 0, 0},
	{0x3704, 0xa0, 0, 0},
	{0x3703, 0x5a, 0, 0},
	{0x3715, 0x78, 0, 0},
	{0x3717, 0x01, 0, 0},
	{0x370b, 0x60, 0, 0},
	{0x3705, 0x1a, 0, 0},
	{0x3905, 0x02, 0, 0},
	{0x3906, 0x10, 0, 0},
	{0x3901, 0x0a, 0, 0},
	{0x3731, 0x12, 0, 0},
	{0x3600, 0x08, 0, 0},
	{0x3601, 0x33, 0, 0},
	{0x302d, 0x60, 0, 0},
	{0x3620, 0x52, 0, 0},
	{0x371b, 0x20, 0, 0},
	{0x471c, 0x50, 0, 0},
	{0x3a13, 0x43, 0, 0},
	{0x3a18, 0x00, 0, 0},
	{0x3a19, 0xf8, 0, 0},
	{0x3635, 0x13, 0, 0},
	{0x3636, 0x03, 0, 0},
	{0x3634, 0x40, 0, 0},
	{0x3622, 0x01, 0, 0},
	{0x3c01, 0x34, 0, 0},
	{0x3c04, 0x28, 0, 0},
	{0x3c05, 0x98, 0, 0},
	{0x3c06, 0x00, 0, 0},
	{0x3c07, 0x07, 0, 0},
	{0x3c08, 0x00, 0, 0},
	{0x3c09, 0x1c, 0, 0},
	{0x3c0a, 0x9c, 0, 0},
	{0x3c0b, 0x40, 0, 0},
	{0x3820, 0x40, 0, 0},
	{0x3821, 0x06, 0, 0},
	{0x3814, 0x11, 0, 0},
	{0x3815, 0x11, 0, 0},
	{0x3800, 0x01, 0, 0},
	{0x3801, 0x50, 0, 0},
	{0x3802, 0x01, 0, 0},
	{0x3803, 0xb2, 0, 0},
	{0x3804, 0x08, 0, 0},
	{0x3805, 0xef, 0, 0},
	{0x3806, 0x05, 0, 0},
	{0x3807, 0xf1, 0, 0},
	{0x3808, 0x07, 0, 0},
	{0x3809, 0x80, 0, 0},
	{0x380a, 0x04, 0, 0},
	{0x380b, 0x38, 0, 0},
	{0x380c, 0x09, 0, 0},
	{0x380d, 0xc4, 0, 0},
	{0x380e, 0x04, 0, 0},
	{0x380f, 0x60, 0, 0},
	{0x3810, 0x00, 0, 0},
	{0x3811, 0x10, 0, 0},
	{0x3812, 0x00, 0, 0},
	{0x3813, 0x04, 0, 0},
	{0x3618, 0x04, 0, 0},
	{0x3612, 0x2b, 0, 0},
	{0x3708, 0x64, 0, 0},
	{0x3709, 0x12, 0, 0},
	{0x370c, 0x00, 0, 0},
	{0x3a02, 0x04, 0, 0},
	{0x3a03, 0x60, 0, 0},
	{0x3a08, 0x01, 0, 0},
	{0x3a09, 0x50, 0, 0},
	{0x3a0a, 0x01, 0, 0},
	{0x3a0b, 0x18, 0, 0},
	{0x3a0e, 0x03, 0, 0},
	{0x3a0d, 0x04, 0, 0},
	{0x3a14, 0x04, 0, 0},
	{0x3a15, 0x60, 0, 0},
	{0x4001, 0x02, 0, 0},
	{0x4004, 0x06, 0, 0},
	{0x3000, 0x00, 0, 0},
	{0x3002, 0x1c, 0, 0},
	{0x3004, 0xff, 0, 0},
	{0x3006, 0xc3, 0, 0},
	{0x300e, 0x45, 0, 0},
	{0x302e, 0x08, 0, 0},
	{0x4300, 0x32, 0, 0}, //zuojisi 0x30 = YUYV, 0x32 = UYVY
	{0x501f, 0x00, 0, 0},
	{0x4713, 0x02, 0, 0},
	{0x4407, 0x04, 0, 0},
	{0x440e, 0x00, 0, 0},
	{0x460b, 0x37, 0, 0},
	{0x460c, 0x20, 0, 0},
	{0x4837, 0x0a, 0, 0},
	{0x3824, 0x04, 0, 0},
	{0x5000, 0xa7, 0, 0},
	{0x5001, 0x83, 0, 0},
	{0x5180, 0xff, 0, 0},
	{0x5181, 0xf2, 0, 0},
	{0x5182, 0x00, 0, 0},
	{0x5183, 0x14, 0, 0},
	{0x5184, 0x25, 0, 0},
	{0x5185, 0x24, 0, 0},
	{0x5186, 0x09, 0, 0},
	{0x5187, 0x09, 0, 0},
	{0x5188, 0x09, 0, 0},
	{0x5189, 0x75, 0, 0},
	{0x518a, 0x54, 0, 0},
	{0x518b, 0xe0, 0, 0},
	{0x518c, 0xb2, 0, 0},
	{0x518d, 0x42, 0, 0},
	{0x518e, 0x3d, 0, 0},
	{0x518f, 0x56, 0, 0},
	{0x5190, 0x46, 0, 0},
	{0x5191, 0xf8, 0, 0},
	{0x5192, 0x04, 0, 0},
	{0x5193, 0x70, 0, 0},
	{0x5194, 0xf0, 0, 0},
	{0x5195, 0xf0, 0, 0},
	{0x5196, 0x03, 0, 0},
	{0x5197, 0x01, 0, 0},
	{0x5198, 0x04, 0, 0},
	{0x5199, 0x12, 0, 0},
	{0x519a, 0x04, 0, 0},
	{0x519b, 0x00, 0, 0},
	{0x519c, 0x06, 0, 0},
	{0x519d, 0x82, 0, 0},
	{0x519e, 0x38, 0, 0},
	{0x5381, 0x1e, 0, 0},
	{0x5382, 0x5b, 0, 0},
	{0x5383, 0x08, 0, 0},
	{0x5384, 0x0a, 0, 0},
	{0x5385, 0x7e, 0, 0},
	{0x5386, 0x88, 0, 0},
	{0x5387, 0x7c, 0, 0},
	{0x5388, 0x6c, 0, 0},
	{0x5389, 0x10, 0, 0},
	{0x538a, 0x01, 0, 0},
	{0x538b, 0x98, 0, 0},
	{0x5300, 0x08, 0, 0},
	{0x5301, 0x30, 0, 0},
	{0x5302, 0x10, 0, 0},
	{0x5303, 0x00, 0, 0},
	{0x5304, 0x08, 0, 0},
	{0x5305, 0x30, 0, 0},
	{0x5306, 0x08, 0, 0},
	{0x5307, 0x16, 0, 0},
	{0x5309, 0x08, 0, 0},
	{0x530a, 0x30, 0, 0},
	{0x530b, 0x04, 0, 0},
	{0x530c, 0x06, 0, 0},
	{0x5480, 0x01, 0, 0},
	{0x5580, 0x02, 0, 0},
	{0x5583, 0x40, 0, 0},
	{0x5584, 0x10, 0, 0},
	{0x5589, 0x10, 0, 0},
	{0x558a, 0x00, 0, 0},
	{0x558b, 0xf8, 0, 0},
	{0x5800, 0x23, 0, 0},
	{0x5801, 0x14, 0, 0},
	{0x5802, 0x0f, 0, 0},
	{0x5803, 0x0f, 0, 0},
	{0x5804, 0x12, 0, 0},
	{0x5805, 0x26, 0, 0},
	{0x5806, 0x0c, 0, 0},
	{0x5807, 0x08, 0, 0},
	{0x5808, 0x05, 0, 0},
	{0x5809, 0x05, 0, 0},
	{0x580a, 0x08, 0, 0},
	{0x580b, 0x0d, 0, 0},
	{0x580c, 0x08, 0, 0},
	{0x580d, 0x03, 0, 0},
	{0x580e, 0x00, 0, 0},
	{0x580f, 0x00, 0, 0},
	{0x5810, 0x03, 0, 0},
	{0x5811, 0x09, 0, 0},
	{0x5812, 0x07, 0, 0},
	{0x5813, 0x03, 0, 0},
	{0x5814, 0x00, 0, 0},
	{0x5815, 0x01, 0, 0},
	{0x5816, 0x03, 0, 0},
	{0x5817, 0x08, 0, 0},
	{0x5818, 0x0d, 0, 0},
	{0x5819, 0x08, 0, 0},
	{0x581a, 0x05, 0, 0},
	{0x581b, 0x06, 0, 0},
	{0x581c, 0x08, 0, 0},
	{0x581d, 0x0e, 0, 0},
	{0x581e, 0x29, 0, 0},
	{0x581f, 0x17, 0, 0},
	{0x5820, 0x11, 0, 0},
	{0x5821, 0x11, 0, 0},
	{0x5822, 0x15, 0, 0},
	{0x5823, 0x28, 0, 0},
	{0x5824, 0x46, 0, 0},
	{0x5825, 0x26, 0, 0},
	{0x5826, 0x08, 0, 0},
	{0x5827, 0x26, 0, 0},
	{0x5828, 0x64, 0, 0},
	{0x5829, 0x26, 0, 0},
	{0x582a, 0x24, 0, 0},
	{0x582b, 0x22, 0, 0},
	{0x582c, 0x24, 0, 0},
	{0x582d, 0x24, 0, 0},
	{0x582e, 0x06, 0, 0},
	{0x582f, 0x22, 0, 0},
	{0x5830, 0x40, 0, 0},
	{0x5831, 0x42, 0, 0},
	{0x5832, 0x24, 0, 0},
	{0x5833, 0x26, 0, 0},
	{0x5834, 0x24, 0, 0},
	{0x5835, 0x22, 0, 0},
	{0x5836, 0x22, 0, 0},
	{0x5837, 0x26, 0, 0},
	{0x5838, 0x44, 0, 0},
	{0x5839, 0x24, 0, 0},
	{0x583a, 0x26, 0, 0},
	{0x583b, 0x28, 0, 0},
	{0x583c, 0x42, 0, 0},
	{0x583d, 0xce, 0, 0},
	{0x5025, 0x00, 0, 0},
	{0x3a0f, 0x30, 0, 0},
	{0x3a10, 0x28, 0, 0},
	{0x3a1b, 0x30, 0, 0},
	{0x3a1e, 0x26, 0, 0},
	{0x3a11, 0x60, 0, 0},
	{0x3a1f, 0x14, 0, 5},

};

static const struct reg_value ov5640_start_stream[] = {
	{0x3008, 0x02, 0, 10}, //WAKE UP
};

static const struct reg_value ov5640_stop_stream[] = {
	{0x3008, 0x42, 0, 10} //SW Power Down, wait 10ms
};

static int ov5640_write_reg(struct ov5640_dev *sensor, u16 reg, u8 val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[3];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "zuojisi: %s: error: slave=%x, reg=%x, val=%x\n",
			__func__, client->addr, reg, val);
		return ret;
	}

	dev_info(&client->dev, "zuojisi: w 0x%02x : (0x%04x)->0x%02x\n",client->addr,reg,val);

	return 0;
}

static int ov5640_read_reg(struct ov5640_dev *sensor, u16 reg, u8 *val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = 1;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_err(&client->dev, "zuojisi: %s: error: slave=%x reg=%x\n",
			__func__, client->addr, reg);
		return ret;
	}

	*val = buf[0];
	dev_info(&client->dev, "zuojisi: w 0x%02x : (0x%04x)<-0x%02x\n",client->addr,reg, *val);
	return 0;
}

static int ov5640_mod_reg(struct ov5640_dev *sensor, u16 reg,
			  u8 mask, u8 val)
{
	u8 readval;
	int ret;

	ret = ov5640_read_reg(sensor, reg, &readval);
	if (ret)
		return ret;

	readval &= ~mask;
	val &= mask;
	val |= readval;

	return ov5640_write_reg(sensor, reg, val);
}


static void ov5640_load_regs(struct ov5640_dev *sensor,
			     const struct reg_value *regs, unsigned int regnum)
{
	unsigned int i;
	u32 delay_ms;
	u16 reg_addr;
	u8 mask, val;
	int ret = 0;

	for (i = 0; i < regnum; ++i, ++regs) {
		delay_ms = regs->delay_ms;
		reg_addr = regs->reg_addr;
		val = regs->val;
		mask = regs->mask;

		if (mask)
			ret = ov5640_mod_reg(sensor, reg_addr, mask, val);
		else
			ret = ov5640_write_reg(sensor, reg_addr, val);
		if (ret)
			break;

		if (delay_ms)
			usleep_range(1000 * delay_ms, 1000 * delay_ms + 100);
	}
}


/* restore the last set video mode after chip power-on */
static int ov5640_init_base_settings(struct ov5640_dev *sensor)
{
	/* first load the initial register values */
	// ov5640_load_regs(sensor, ov5640_init_setting, ARRAY_SIZE(ov5640_init_setting));

	return 0;
}

static void ov5640_power(struct ov5640_dev *sensor, bool enable)
{
	gpiod_set_value_cansleep(sensor->pwdn_gpio, enable ? 0 : 1);
}

/*
 * From section 2.7 power up sequence:
 * t0 + t1 + t2 >= 5ms	Delay from DOVDD stable to PWDN pull down
 * t3 >= 1ms		Delay from PWDN pull down to RESETB pull up
 * t4 >= 20ms		Delay from RESETB pull up to SCCB (i2c) stable
 *
 * Some modules don't expose RESETB/PWDN pins directly, instead providing a
 * "PWUP" GPIO which is wired through appropriate delays and inverters to the
 * pins.
 *
 * In such cases, this gpio should be mapped to pwdn_gpio in the driver, and we
 * should still toggle the pwdn_gpio below with the appropriate delays, while
 * the calls to reset_gpio will be ignored.
 */
static void ov5640_powerup_sequence(struct ov5640_dev *sensor)
{
	if (sensor->pwdn_gpio) {
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);

		/* camera power cycle */
		ov5640_power(sensor, false);
		usleep_range(5000, 10000);
		ov5640_power(sensor, true);
		usleep_range(5000, 10000);

		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
		usleep_range(1000, 2000);

		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	}
	usleep_range(20000, 25000);

}

static int ov5640_set_power_on(struct ov5640_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		return ret;
	}

	ret = regulator_bulk_enable(OV5640_NUM_SUPPLIES,
				    sensor->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		goto xclk_off;
	}

	ov5640_powerup_sequence(sensor);

	return 0;

xclk_off:
	clk_disable_unprepare(sensor->xclk);
	return ret;
}

static void ov5640_set_power_off(struct ov5640_dev *sensor)
{
	ov5640_power(sensor, false);
	regulator_bulk_disable(OV5640_NUM_SUPPLIES, sensor->supplies);
	clk_disable_unprepare(sensor->xclk);
}


static int ov5640_set_power(struct ov5640_dev *sensor, bool on)
{
	int ret = 0;

	if (on) {
		ret = ov5640_set_power_on(sensor);
		if (ret)
			return ret;

		ret = ov5640_init_base_settings(sensor);
		if (ret)
			goto power_off;
	}

	if (!on)
		ov5640_set_power_off(sensor);

	return 0;

power_off:
	ov5640_set_power_off(sensor);
	return ret;
}

static int ov5640_sensor_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov5640_dev *ov5640 = to_ov5640_dev(sd);

	return ov5640_set_power(ov5640, false);
}

static int ov5640_sensor_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov5640_dev *ov5640 = to_ov5640_dev(sd);

	return ov5640_set_power(ov5640, true);
}

/* --------------- Subdev Operations --------------- */
static int ov5640_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s , pad=%d\n",__func__, format->pad);

	if (format->pad > 1)
		return -EINVAL;

	mutex_lock(&sensor->lock);
	format->format = ov5640_default_fmt;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int ov5640_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	int ret = 0;
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);
	dev_info(&sensor->i2c_client->dev, "zuojisi: which=%s", format->which ? "V4L2_SUBDEV_FORMAT_ACTIVE":"V4L2_SUBDEV_FORMAT_TRY");
	dev_info(&sensor->i2c_client->dev, "zuojisi: code=%d, colorspace=%d",format->format.code, format->format.colorspace);
	dev_info(&sensor->i2c_client->dev, "zuojisi: xfer_func=%d, ycbcr_enc=%d",format->format.xfer_func, format->format.ycbcr_enc);
	dev_info(&sensor->i2c_client->dev, "zuojisi: quantization=%d, field=%d",format->format.quantization, format->format.field);
	dev_info(&sensor->i2c_client->dev, "zuojisi: width=%d, height=%d",format->format.width, format->format.height);

	if (format->pad > 1)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, 0) = format->format;
		goto out;
	}

	ov5640_default_fmt = format->format;

	ret = 0;
out:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int ov5640_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct ov5640_dev *sensor = to_ov5640_dev(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		mutex_lock(&sensor->lock);
		sel->r.left = sel->r.top = 0;
		sel->r.width = 1920;
		sel->r.height = 1080;
		mutex_unlock(&sensor->lock);
		return 0;
	}

	return -EINVAL;
}

static int ov5640_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned int index = fse->index;
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	if (fse->pad > 1)
		return -EINVAL;

	if (index != 0)
		return -EINVAL;

	fse->min_width = 1920;
	fse->max_width = fse->min_width;
	fse->min_height = 1080;
	fse->max_height = fse->min_height;

	return 0;
}

static int ov5640_enum_frame_interval(
	struct v4l2_subdev *sd,
	struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval_enum *fie)
{
	struct v4l2_fract tpf;
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s , pad=%d, index=%d\n",__func__, fie->pad,fie->index);

	if (fie->pad > 1)
		return -EINVAL;
	if (fie->index != 0)
		return -EINVAL;

	tpf.numerator = 1;
	tpf.denominator = 30;

	fie->interval = tpf;
	return 0;
}

static int ov5640_get_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	mutex_lock(&sensor->lock);
	fi->interval.numerator = 1;
	fi->interval.denominator = 30;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int ov5640_set_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	int ret = 0;
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	if (fi->pad > 1)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}

	mutex_unlock(&sensor->lock);
	return 0; //fix 30fps
out:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int ov5640_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;

	code->code = FMT_CODE;

	return 0;
}

static int ov5640_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	int ret = 0;
	u16 addrbackup;
	int i;

	dev_info(&sensor->i2c_client->dev, "zuojisi: s stream enable=%d\n",enable);

	if (enable) {
		ret = pm_runtime_resume_and_get(&sensor->i2c_client->dev);
		if (ret < 0)
			return ret;
	}

	mutex_lock(&sensor->lock);
	if (sensor->streaming == enable) {
		mutex_unlock(&sensor->lock);
		return 0;
	}

	addrbackup = sensor->i2c_client->addr;
	if (enable) {
		for(i=0;i<2;i++){
			if(sensor->link_lock_status & (1<<i)){
				sensor->i2c_client->addr = 0x48;
				ov5640_write_reg(sensor, 0x0010, 0x21+i);
				mdelay(100);

				sensor->i2c_client->addr = addrbackup;
				ov5640_load_regs(sensor, ov5640_init_setting, ARRAY_SIZE(ov5640_init_setting));
				mdelay(50);
				ov5640_load_regs(sensor, ov5640_start_stream, ARRAY_SIZE(ov5640_start_stream));
			}
		}

		sensor->i2c_client->addr = 0x48;
		ov5640_write_reg(sensor, 0x0010, 0x20+sensor->link_lock_status);
		usleep_range(1000 * 100, 1000 * 100 + 100);

		for(i=0;i<2;i++){
			if(sensor->link_lock_status & (1<<i)){
				sensor->i2c_client->addr = 0x40;
				ov5640_write_reg(sensor, 0x0010, 0x21);
				usleep_range(1000 * 100, 1000 * 100 + 100);
			}
		}

		sensor->i2c_client->addr = addrbackup;
	} else {
		for(i=0;i<2;i++){
			if(sensor->link_lock_status & (1<<i)){
				sensor->i2c_client->addr = 0x48;
				ov5640_write_reg(sensor, 0x0010, 0x21+i);
				mdelay(100);

				sensor->i2c_client->addr = addrbackup;
				ov5640_load_regs(sensor, ov5640_stop_stream, ARRAY_SIZE(ov5640_stop_stream));
			}
		}
	}


	sensor->streaming = enable;
	mutex_unlock(&sensor->lock);

	if (!enable || ret)
		pm_runtime_put_autosuspend(&sensor->i2c_client->dev);

	return ret;
}

static int ov5640_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt =
				v4l2_subdev_state_get_format(state, 0);
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);

	*fmt = ov5640_default_fmt;

	crop->left = 0;
	crop->top = 0;
	crop->width = 1920;
	crop->height = 1080;

	return 0;
}

static int ov5640_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad, struct v4l2_mbus_frame_desc *fd)
{
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].bus.csi2.dt = CAM_STREAM_FORMAT;
	fd->entry[0].bus.csi2.vc = (u8)pad;
	return 0;
}

static const struct v4l2_subdev_core_ops ov5640_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops ov5640_video_ops = {
	.s_stream = ov5640_s_stream,
};

static const struct v4l2_subdev_pad_ops ov5640_pad_ops = {
	.enum_mbus_code = ov5640_enum_mbus_code,
	.get_fmt = ov5640_get_fmt,
	.set_fmt = ov5640_set_fmt,
	.get_selection = ov5640_get_selection,
	.enum_frame_size = ov5640_enum_frame_size,
	.enum_frame_interval = ov5640_enum_frame_interval,
	.get_frame_desc = ov5640_get_frame_desc,
	.get_frame_interval = ov5640_get_frame_interval,
	.set_frame_interval = ov5640_set_frame_interval,
};

static const struct v4l2_subdev_internal_ops ov5640_internal_ops = {
	.init_state = ov5640_init_cfg,
};

static const struct v4l2_subdev_ops ov5640_subdev_ops = {
	.core = &ov5640_core_ops,
	.video = &ov5640_video_ops,
	.pad = &ov5640_pad_ops,
};

static int ov5640_get_regulators(struct ov5640_dev *sensor)
{
	int i;

	for (i = 0; i < OV5640_NUM_SUPPLIES; i++)
		sensor->supplies[i].supply = ov5640_supply_name[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       OV5640_NUM_SUPPLIES,
				       sensor->supplies);
}

static int ov5640_setup_deserializer(struct ov5640_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	struct device *dev = &client->dev;
	u16 addrbackup = client->addr;
	u8 val = 0;
	int ret,i;
	client->addr = 0x48;

	ret = ov5640_read_reg(sensor, 0x000d, &val);
	if(ret == 0 && val == 0x94){
		dev_info(dev, "zuojisi: deserializer detected\n");
	}else{
		dev_err(dev, "zuojisi: deserializer NOT detected\n");
		goto error;
	}

	ov5640_write_reg(sensor, 0x0003, 0x40);
	ov5640_write_reg(sensor, 0x02C2, 0x00);
	ov5640_write_reg(sensor, 0x02C3, 0x00);
	ov5640_write_reg(sensor, 0x0076, 0x70);
	ov5640_write_reg(sensor, 0x007e, 0x70);
	ov5640_write_reg(sensor, 0x0018, 0x0c);

	sensor->link_lock_status = 0;
	for(i=0;i<2;i++){
		client->addr = 0x48;
		ov5640_write_reg(sensor, 0x0010, 0x21+i);
		mdelay(100);

		client->addr = 0x40;

		ret = ov5640_read_reg(sensor, 0x000d, &val);
		if(ret == 0 && (val == 0x91 || val == 0xbf)){
			sensor->link_lock_status |= 1<<i;
			dev_info(dev, "zuojisi: serializer detected on port %d\n",i);
		}else{
			dev_err(dev, "zuojisi: serializer NOT detected on port %d\n",i);
			continue;
		}

		ov5640_write_reg(sensor, 0x02be, 0x10);
		ov5640_write_reg(sensor, 0x02bf, 0x60);
		ov5640_write_reg(sensor, 0x0311, 0x40); //z
		ov5640_write_reg(sensor, 0x0308, 0x64); //z
		ov5640_write_reg(sensor, 0x0002, 0x43); //Z

		if(i==0){
			ov5640_write_reg(sensor, 0x005b, 0x2); //Z
		}else{
			ov5640_write_reg(sensor, 0x0053, 0x3); //X as U , no use
			ov5640_write_reg(sensor, 0x005b, 0x0); //Z as X
		}
		ov5640_write_reg(sensor, 0x0334, 0x70); //phy1 for xcSer inverse polarity
		ov5640_write_reg(sensor, 0x0335, 0x07); //phy2 for xcSer inverse polarity
		ov5640_write_reg(sensor, 0x0331, 0x11); //2 lanes

		ov5640_write_reg(sensor, 0x007b, 0x30+i);
		ov5640_write_reg(sensor, 0x0083, 0x30+i);
		ov5640_write_reg(sensor, 0x0093, 0x30+i);
		ov5640_write_reg(sensor, 0x009b, 0x30+i);
		ov5640_write_reg(sensor, 0x00a3, 0x30+i);
		ov5640_write_reg(sensor, 0x00ab, 0x30+i);
		ov5640_write_reg(sensor, 0x008b, 0x30+i);
	}
	if(sensor->link_lock_status ==0){
		return -EINVAL;
	}

	client->addr = 0x48;
	ov5640_write_reg(sensor, 0x0010, 0x20+sensor->link_lock_status);
	mdelay(30);

		//pipe-Z
	ov5640_write_reg(sensor, 0x048b, 0x07);
	ov5640_write_reg(sensor, 0x04ad, 0x15);
	ov5640_write_reg(sensor, 0x048d, CAM_STREAM_FORMAT);
	ov5640_write_reg(sensor, 0x048e, CAM_STREAM_FORMAT|(0<<6));
	ov5640_write_reg(sensor, 0x048f, 0x00);
	ov5640_write_reg(sensor, 0x0490, 0x00|(0<<6));
	ov5640_write_reg(sensor, 0x0491, 0x01);
	ov5640_write_reg(sensor, 0x0492, 0x01|(0<<6));


	ov5640_write_reg(sensor, 0x040b, 0x07);
	ov5640_write_reg(sensor, 0x042d, 0x15);
	ov5640_write_reg(sensor, 0x040d, CAM_STREAM_FORMAT);
	ov5640_write_reg(sensor, 0x040e, CAM_STREAM_FORMAT|(1<<6));
	ov5640_write_reg(sensor, 0x040f, 0x00);
	ov5640_write_reg(sensor, 0x0410, 0x00|(1<<6));
	ov5640_write_reg(sensor, 0x0411, 0x01);
	ov5640_write_reg(sensor, 0x0412, 0x01|(1<<6));

	ov5640_write_reg(sensor, 0x0050, 0x00);
	ov5640_write_reg(sensor, 0x0052, 0x02);
	ov5640_write_reg(sensor, 0x044a, 0x40);
	ov5640_write_reg(sensor, 0x0320, 0x2f); //1500MHz


	usleep_range(1000 * 300, 1000 * 300 + 100);
	
	client->addr = addrbackup;
	return 0;
error:
	client->addr = addrbackup;
	return -EINVAL;
}

static ssize_t zuojisi_links_show(struct device *dev, struct device_attribute*attr, char *buf)  
{
    u32 idx;
	struct ov5640_dev* sensor =  v4l2_get_subdevdata(dev_get_drvdata(dev));

	dev_info(dev, "zuojisi: get link status=0x%x\n", sensor->link_lock_status);

    for(idx=0;idx<4;idx++){
        if( sensor->link_lock_status & (1<<idx)){
            buf[2*idx] = '1';
            buf[2*idx+1] = ' ';
        }else{
            buf[2*idx] = '0';
            buf[2*idx+1] = ' ';
        }
    }
    buf[8] = 0;
    return 9;
}
 
static DEVICE_ATTR(zuojisi_links, S_IRUSR |S_IRUGO, zuojisi_links_show, NULL);  

static int ov5640_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct ov5640_dev *sensor;
	int ret;

	dev_info(dev, "zuojisi: probing ov5640....\n");

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev),
						  NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		return ret;
	}

	if (sensor->ep.bus_type != V4L2_MBUS_PARALLEL &&
	    sensor->ep.bus_type != V4L2_MBUS_CSI2_DPHY &&
	    sensor->ep.bus_type != V4L2_MBUS_BT656) {
		dev_err(dev, "Unsupported bus type %d\n", sensor->ep.bus_type);
		return -EINVAL;
	}

	/* get system clock (xclk) */
	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(sensor->xclk);
	}

	/* request optional power down pin */
	sensor->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn_gpio))
		return PTR_ERR(sensor->pwdn_gpio);

	/* request optional reset pin */
	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return PTR_ERR(sensor->reset_gpio);

	v4l2_i2c_subdev_init(&sensor->sd, client, &ov5640_subdev_ops);
	v4l2_set_subdevdata(&sensor->sd,sensor);
	sensor->sd.internal_ops = &ov5640_internal_ops;

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	sensor->pad[0].flags = MEDIA_PAD_FL_SOURCE;
	sensor->pad[1].flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 2, sensor->pad);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto entity_cleanup;

	ret = ov5640_get_regulators(sensor);
	if (ret)
		goto entity_cleanup;

	mutex_init(&sensor->lock);

	ret = ov5640_setup_deserializer(sensor);
	if (ret) {
		dev_err(dev, "failed to setup deserializer\n");
		goto entity_cleanup;
	}

	ret = ov5640_sensor_resume(dev);
	if (ret) {
		dev_err(dev, "failed to power on\n");
		goto entity_cleanup;
	}

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_pm_runtime;

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	device_create_file(dev,&dev_attr_zuojisi_links);

	dev_info(dev, "zuojisi: probe ov5640 success!\n");

	return 0;

err_pm_runtime:
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	ov5640_sensor_suspend(dev);
entity_cleanup:
	v4l2_subdev_cleanup(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	mutex_destroy(&sensor->lock);
	return ret;
}

static void ov5640_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5640_dev *sensor = to_ov5640_dev(sd);
	struct device *dev = &client->dev;

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		ov5640_sensor_suspend(dev);
	pm_runtime_set_suspended(dev);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_subdev_cleanup(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	mutex_destroy(&sensor->lock);
}

static const struct dev_pm_ops ov5640_pm_ops = {
	SET_RUNTIME_PM_OPS(ov5640_sensor_suspend, ov5640_sensor_resume, NULL)
};

static const struct i2c_device_id ov5640_id[] = {
	{"ov5640", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ov5640_id);

static const struct of_device_id ov5640_dt_ids[] = {
	{ .compatible = "zuojisi,z_xcdes17_xcser17_ov5640" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov5640_dt_ids);

static struct i2c_driver ov5640_i2c_driver = {
	.driver = {
		.name  = "z_xcdes17_xcser17_ov5640",
		.of_match_table	= ov5640_dt_ids,
		.pm = &ov5640_pm_ops,
	},
	.id_table = ov5640_id,
	.probe    = ov5640_probe,
	.remove   = ov5640_remove,
};

module_i2c_driver(ov5640_i2c_driver);

MODULE_DESCRIPTION("OV5640 MIPI Camera Subdev Driver by zuojisi@163.com");
MODULE_LICENSE("GPL");
