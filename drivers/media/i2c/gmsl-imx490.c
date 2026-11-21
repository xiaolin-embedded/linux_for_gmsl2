// SPDX-License-Identifier: GPL-2.0

#include "gmsl-core.h"
#include "gmsl-imx490.h"

static int imx490_sensor_init(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;

	if (!link->desc->init || !link->desc->init_len)
		return 0;

	return gmsl_load_regs(gmsl, link->sensor_addr, link->desc->init,
			      link->desc->init_len);
}

static int imx490_start_stream(struct gmsl_link *link, bool on)
{
	struct gmsl_dev *gmsl = link->dev;
	const struct gmsl_reg_value *regs;
	unsigned int len;

	regs = on ? link->desc->start : link->desc->stop;
	len = on ? link->desc->start_len : link->desc->stop_len;
	if (!regs || !len)
		return 0;

	return gmsl_load_regs(gmsl, link->sensor_addr, regs, len);
}

static int imx490_get_fmt(struct gmsl_link *link,
			  struct v4l2_mbus_framefmt *fmt)
{
	*fmt = link->fmt;
	return 0;
}

static int imx490_get_mbus_config(struct gmsl_link *link,
				  struct v4l2_mbus_config *cfg)
{
	*cfg = link->dev->mbus_config;
	if (!cfg->link_freq)
		cfg->link_freq = link->link_freq;

	return 0;
}

const struct gmsl_sensor_ops imx490_sensor_ops = {
	.name = "imx490",
	.init = imx490_sensor_init,
	.start_stream = imx490_start_stream,
	.get_fmt = imx490_get_fmt,
	.get_mbus_config = imx490_get_mbus_config,
};
