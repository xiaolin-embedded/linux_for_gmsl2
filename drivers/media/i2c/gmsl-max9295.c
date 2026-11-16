// SPDX-License-Identifier: GPL-2.0

#include "gmsl-core.h"
#include "gmsl-max9295.h"

#define MAX9295_I2C_ADDR		0x40
#define MAX9295_DEV_ID			0x91
#define MAX9295_GPIO_FRAME_TRIGGER	0x02d3

static int max9295_probe(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	u8 id = 0;
	int ret;

	ret = gmsl_read_addr(gmsl, MAX9295_I2C_ADDR, 0x000d, &id, true);
	if (ret == 0 && id == MAX9295_DEV_ID) {
		gmsl_write_addr(gmsl, MAX9295_I2C_ADDR, 0x0000,
				(MAX9295_I2C_ADDR + link->index + 1) << 1);
		mdelay(75);
	}

	link->ser_addr = MAX9295_I2C_ADDR + link->index + 1;
	mdelay(75);

	ret = gmsl_read_addr(gmsl, link->ser_addr, 0x000d, &id, false);
	if (ret || id != MAX9295_DEV_ID) {
		dev_err(gmsl->dev,
			"max96724-imx390: serializer NOT detected\n");
		return -ENODEV;
	}

	dev_info(gmsl->dev, "max96724-imx390: serializer detected\n");
	return 0;
}

static int max9295_configure(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	u8 addr = link->ser_addr;
	u8 dt = link->desc->csi_dt;
	u8 id = 0x30 + link->index;

	gmsl_write_addr(gmsl, addr, 0x02bf, 0x60);
	gmsl_write_addr(gmsl, addr, 0x02be, 0x10);
	gmsl_write_addr(gmsl, addr, 0x0318, 0x40 | dt);
	gmsl_write_addr(gmsl, addr, 0x0311, 0x40);
	gmsl_write_addr(gmsl, addr, 0x0308, 0x64);
	gmsl_write_addr(gmsl, addr, 0x0002, 0x43);
	gmsl_write_addr(gmsl, addr, 0x0331, 0x33); /* 4 lanes */

	gmsl_write_addr(gmsl, addr, 0x007b, id);
	gmsl_write_addr(gmsl, addr, 0x0083, id);
	gmsl_write_addr(gmsl, addr, 0x0093, id);
	gmsl_write_addr(gmsl, addr, 0x009b, id);
	gmsl_write_addr(gmsl, addr, 0x00a3, id);
	gmsl_write_addr(gmsl, addr, 0x00ab, id);
	gmsl_write_addr(gmsl, addr, 0x008b, id);

	return 0;
}

static int max9295_gpio_frame_sync(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	u8 addr = link->ser_addr;

	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 1, 0xa0);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3 + 1, 0xa0);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER, 0x00);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3, 0x00);
	mdelay(50);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER, 0x10);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3, 0x10);
	mdelay(50);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER, 0x00);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3, 0x00);
	mdelay(50);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER, 0x10);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3, 0x10);
	mdelay(50);

	/* receive remote */
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER, 0x04);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 1, 0x6a);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 2, 0x0a);

	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3, 0x04);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3 + 1, 0x6a);
	gmsl_write_addr(gmsl, addr, MAX9295_GPIO_FRAME_TRIGGER + 3 + 2, 0x0a);

	return 0;
}

const struct gmsl_ser_ops max9295_ser_ops = {
	.name = "max9295",
	.id = MAX9295_DEV_ID,
	.probe = max9295_probe,
	.configure = max9295_configure,
	.gpio_frame_sync = max9295_gpio_frame_sync,
};
