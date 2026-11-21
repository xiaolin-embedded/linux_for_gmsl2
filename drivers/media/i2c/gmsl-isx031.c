// SPDX-License-Identifier: GPL-2.0

#include "gmsl-core.h"
#include "gmsl-isx031.h"

#define ISX031_REG_MODE_SELECT		0x8a00
#define ISX031_REG_MODE_SET_F		0x8a01
#define ISX031_REG_MODE_SET_F_LOCK	0xbef0
#define ISX031_MODE_UNLOCK		0x53
#define ISX031_MODE_STANDBY		0x00
#define ISX031_MODE_STREAMING		0x80
#define ISX031_MODE_4LANES_30FPS	0x17

static const struct gmsl_reg_value isx031_init_regs[] = {
	{ 0xffff, 0x00 },
	{ 0x0171, 0x00 },
	{ 0x0172, 0x00 },
	{ 0xbf14, 0x00 },
	{ 0x8af0, 0x00 },
};

static const struct gmsl_reg_value isx031_1920x1536_regs[] = {
	{ 0x8aa8, 0x01 },
	{ 0x8aaa, 0x80 },
	{ 0x8aab, 0x07 },
	{ 0x8aac, 0x00 },
	{ 0x8aad, 0x00 },
	{ 0x8aae, 0x00 },
	{ 0x8aaf, 0x06 },
	{ 0x8ab0, 0x00 },
	{ 0x8ab1, 0x00 },
	{ 0x8ada, 0x03 },
	{ 0xbf04, 0x01 },
	{ 0xbf06, 0x80 },
	{ 0xbf07, 0x07 },
	{ 0xbf08, 0x00 },
	{ 0xbf09, 0x00 },
	{ 0xbf0a, 0x00 },
	{ 0xbf0b, 0x06 },
	{ 0xbf0c, 0x00 },
	{ 0xbf0d, 0x00 },
};

static int isx031_write_sensor(struct gmsl_link *link, u16 reg, u8 val)
{
	struct gmsl_dev *gmsl = link->dev;
	int ret = -EIO;
	int i;

	for (i = 0; i < 5; i++) {
		ret = gmsl_write_addr(gmsl, link->sensor_addr, reg, val);
		if (!ret)
			return 0;
		msleep(20);
	}

	return ret;
}

static int isx031_load_sensor_regs(struct gmsl_link *link,
				   const struct gmsl_reg_value *regs,
				   unsigned int num)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num; i++) {
		ret = isx031_write_sensor(link, regs[i].reg_addr, regs[i].val);
		if (ret)
			return ret;
		if (regs[i].delay_ms)
			msleep(regs[i].delay_ms);
	}

	return 0;
}

static int isx031_ser_init(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	u8 addr = link->ser_addr;
	u8 val;
	int ret;

	/* The sensor sits on the serializer's pass-through I2C bus. */
	ret = gmsl_write_addr(gmsl, addr, 0x0001, 0xc8);
	if (ret)
		return ret;
	usleep_range(20, 30);

	ret = gmsl_write_addr(gmsl, addr, 0x0003, 0x03);
	if (ret)
		return ret;

	ret = gmsl_write_addr(gmsl, addr, 0x0006, 0xb0);
	if (ret)
		return ret;

	ret = gmsl_write_addr(gmsl, addr, 0x03f0, 0x59);
	if (ret)
		return ret;

	ret = gmsl_write_addr(gmsl, addr, 0x03f1, 0x09);
	if (ret)
		return ret;

	if (!gmsl_read_addr(gmsl, addr, 0x0570, &val, true))
		ret = gmsl_write_addr(gmsl, addr, 0x0570, val & ~0x0c);
	else
		ret = gmsl_write_addr(gmsl, addr, 0x0570, 0x30);
	if (ret)
		return ret;

	/* Release the sensor reset line on MFP0. */
	ret = gmsl_write_addr(gmsl, addr, 0x02be, 0x80);
	if (ret)
		return ret;
	msleep(10);

	ret = gmsl_write_addr(gmsl, addr, 0x02be, 0x90);
	if (ret)
		return ret;
	msleep(100);

	return 0;
}

static int isx031_sensor_init(struct gmsl_link *link)
{
	int ret;

	ret = isx031_ser_init(link);
	if (ret)
		return ret;

	ret = isx031_write_sensor(link, ISX031_REG_MODE_SET_F_LOCK,
				  ISX031_MODE_UNLOCK);
	if (ret)
		return ret;

	ret = isx031_write_sensor(link, ISX031_REG_MODE_SET_F,
				  ISX031_MODE_STANDBY);
	if (ret)
		return ret;
	msleep(50);

	ret = isx031_load_sensor_regs(link, isx031_init_regs,
				      ARRAY_SIZE(isx031_init_regs));
	if (ret)
		return ret;

	ret = isx031_load_sensor_regs(link, isx031_1920x1536_regs,
				      ARRAY_SIZE(isx031_1920x1536_regs));
	if (ret)
		return ret;

	ret = isx031_write_sensor(link, ISX031_REG_MODE_SET_F_LOCK,
				  ISX031_MODE_UNLOCK);
	if (ret)
		return ret;

	return isx031_write_sensor(link, ISX031_REG_MODE_SELECT,
				   ISX031_MODE_4LANES_30FPS);
}

static int isx031_start_stream(struct gmsl_link *link, bool on)
{
	int ret;

	if (!on)
		return isx031_write_sensor(link, ISX031_REG_MODE_SET_F,
					   ISX031_MODE_STANDBY);

	ret = isx031_write_sensor(link, ISX031_REG_MODE_SET_F_LOCK,
				  ISX031_MODE_UNLOCK);
	if (ret)
		return ret;

	ret = isx031_write_sensor(link, ISX031_REG_MODE_SET_F,
				  ISX031_MODE_STREAMING);
	if (ret)
		return ret;

	msleep(20);
	return 0;
}

static int isx031_get_fmt(struct gmsl_link *link,
			  struct v4l2_mbus_framefmt *fmt)
{
	*fmt = link->fmt;
	return 0;
}

static int isx031_get_mbus_config(struct gmsl_link *link,
				  struct v4l2_mbus_config *cfg)
{
	*cfg = link->dev->mbus_config;
	if (!cfg->link_freq)
		cfg->link_freq = link->link_freq;

	return 0;
}

const struct gmsl_sensor_ops isx031_sensor_ops = {
	.name = "isx031",
	.init = isx031_sensor_init,
	.start_stream = isx031_start_stream,
	.get_fmt = isx031_get_fmt,
	.get_mbus_config = isx031_get_mbus_config,
};