// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/string.h>

#include "gmsl-core.h"

int gmsl_write_addr(struct gmsl_dev *gmsl, u8 addr, u16 reg, u8 val)
{
	struct i2c_client *client = gmsl->client;
	struct i2c_msg msg;
	u8 buf[3];
	int ret, retry;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val;

	msg.addr = addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	for (retry = 0; retry < 3; retry++) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			break;
		usleep_range(1000, 2000);
	}
	if (ret != 1) {
		dev_err(&client->dev,
			"max96724-imx390: %s: error: slave=%x, reg=%x, val=%x\n",
			__func__, addr, reg, val);
		return ret < 0 ? ret : -EIO;
	}

	dev_dbg(&client->dev,
		"max96724-imx390: w 0x%02x : (0x%04x)->0x%02x\n",
		addr, reg, val);

	return 0;
}

int gmsl_read_addr(struct gmsl_dev *gmsl, u8 addr, u16 reg, u8 *val,
		   bool quiet)
{
	struct i2c_client *client = gmsl->client;
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret, retry;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = 1;

	for (retry = 0; retry < 3; retry++) {
		ret = i2c_transfer(client->adapter, msg, 2);
		if (ret == 2)
			break;
		usleep_range(1000, 2000);
	}
	if (ret != 2) {
		if (!quiet)
			dev_err(&client->dev,
				"max96724-imx390: %s: error: slave=%x reg=%x\n",
				__func__, addr, reg);
		return ret < 0 ? ret : -EIO;
	}

	*val = buf[0];
	dev_dbg(&client->dev,
		"max96724-imx390: r 0x%02x : (0x%04x)<-0x%02x\n",
		addr, reg, *val);

	return 0;
}

int gmsl_mod_reg_addr(struct gmsl_dev *gmsl, u8 addr, u16 reg, u8 mask,
		      u8 val)
{
	u8 readval;
	int ret;

	ret = gmsl_read_addr(gmsl, addr, reg, &readval, false);
	if (ret)
		return ret;

	readval &= ~mask;
	val &= mask;
	val |= readval;

	return gmsl_write_addr(gmsl, addr, reg, val);
}

int gmsl_load_regs(struct gmsl_dev *gmsl, u8 addr,
		   const struct gmsl_reg_value *regs, unsigned int regnum)
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
			ret = gmsl_mod_reg_addr(gmsl, addr, reg_addr, mask, val);
		else
			ret = gmsl_write_addr(gmsl, addr, reg_addr, val);
		if (ret)
			break;

		if (delay_ms)
			usleep_range(1000 * delay_ms, 1000 * delay_ms + 100);
	}

	return ret;
}

static void gmsl_fmt_from_desc(struct v4l2_mbus_framefmt *fmt,
			       const struct gmsl_sensor_desc *desc)
{
	fmt->code = desc->mbus_code;
	fmt->width = desc->width;
	fmt->height = desc->height;
	fmt->colorspace = V4L2_COLORSPACE_REC709;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_REC709);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(V4L2_COLORSPACE_REC709);
	fmt->field = V4L2_FIELD_NONE;
}

int gmsl_core_set_board(struct gmsl_dev *gmsl,
			const struct gmsl_board_desc *board)
{
	const struct gmsl_link_desc *ld;
	struct gmsl_link *link, *first = NULL;
	int i, j;

	if (!board || !board->des_ops || !board->links || !board->num_links)
		return -EINVAL;

	gmsl->board = board;
	gmsl->des_ops = board->des_ops;
	gmsl->dt_addr = gmsl->client->addr;

	for (i = 0; i < GMSL_MAX_LINKS; i++) {
		link = &gmsl->links[i];
		link->dev = gmsl;
		link->index = i;
		link->desc = NULL;
		link->ser_ops = NULL;
		link->ser_addr = 0;
		link->sensor_addr = 0;
		link->num_lanes = 0;
		link->link_freq = 0;
		link->locked = false;
		memset(&link->fmt, 0, sizeof(link->fmt));
	}

	for (j = 0; j < board->num_links; j++) {
		ld = &board->links[j];
		if (!ld->sensor || !ld->sensor->sensor_ops || !ld->ser_ops ||
		    ld->index >= GMSL_MAX_LINKS)
			return -EINVAL;

		link = &gmsl->links[ld->index];
		if (link->desc)
			return -EINVAL;

		link->desc = ld->sensor;
		link->ser_ops = ld->ser_ops;
		link->sensor_addr = ld->sensor_addr;
		link->num_lanes = ld->num_lanes;
		link->link_freq = ld->link_freq;
		gmsl_fmt_from_desc(&link->fmt, ld->sensor);
		if (!first)
			first = link;
	}

	if (!first)
		return -EINVAL;

	/* Keep all exposed pads reporting a valid default format */
	for (i = 0; i < GMSL_MAX_LINKS; i++) {
		link = &gmsl->links[i];
		if (!link->desc)
			link->fmt = first->fmt;
	}

	return 0;
}

int gmsl_core_hw_init(struct gmsl_dev *gmsl)
{
	struct gmsl_link *link;
	int i, ret;

	ret = gmsl->des_ops->init(gmsl);
	if (ret)
		goto out;

	for (i = 0; i < GMSL_MAX_LINKS; i++) {
		link = &gmsl->links[i];
		if (!link->desc || !(gmsl->link_lock_status & BIT(i)))
			continue;

		ret = gmsl->des_ops->configure_link(gmsl, link);
		if (ret)
			goto out;

		ret = gmsl->des_ops->monopolize_link(gmsl, i);
		if (ret)
			goto out;

		ret = link->ser_ops->probe(link);
		if (ret) {
			gmsl->link_lock_status &= ~BIT(i);
			goto out;
		}

		ret = link->ser_ops->configure(link);
		if (ret)
			goto out;

		link->locked = true;
	}

	ret = gmsl->des_ops->restore_link(gmsl);

out:
	gmsl->client->addr = gmsl->dt_addr;
	return ret;
}

int gmsl_core_sensor_init(struct gmsl_dev *gmsl)
{
	struct gmsl_link *link;
	int i, ret;

	for (i = 0; i < GMSL_MAX_LINKS; i++) {
		link = &gmsl->links[i];
		if (!link->locked || !link->desc ||
		    !link->desc->sensor_ops->init)
			continue;

		ret = link->desc->sensor_ops->init(link);
		if (ret)
			return ret;
	}

	gmsl->client->addr = gmsl->dt_addr;
	return 0;
}

int gmsl_core_s_stream(struct gmsl_dev *gmsl, bool enable)
{
	struct gmsl_link *link;
	u8 mask = gmsl->link_lock_status;
	int i, ret = 0;

	if (enable) {
		for (i = 0; i < GMSL_MAX_LINKS; i++) {
			link = &gmsl->links[i];
			if (!(mask & BIT(i)) || !link->desc ||
			    !link->desc->sensor_ops->start_stream)
				continue;

			ret = link->desc->sensor_ops->start_stream(link, true);
			if (ret)
				goto out;
		}

		ret = gmsl->des_ops->enable_pipes(gmsl, mask, true);
		if (ret)
			goto out;

		for (i = 0; i < GMSL_MAX_LINKS; i++) {
			link = &gmsl->links[i];
			if (!(mask & BIT(i)) || !link->ser_ops ||
			    !link->ser_ops->gpio_frame_sync)
				continue;

			ret = link->ser_ops->gpio_frame_sync(link);
			if (ret)
				goto out;
		}
	} else {
		for (i = 0; i < GMSL_MAX_LINKS; i++) {
			link = &gmsl->links[i];
			if (!(mask & BIT(i)) || !link->desc ||
			    !link->desc->sensor_ops->start_stream)
				continue;

			ret = link->desc->sensor_ops->start_stream(link, false);
			if (ret)
				goto out;
		}
	}

out:
	gmsl->client->addr = gmsl->dt_addr;
	return ret;
}
