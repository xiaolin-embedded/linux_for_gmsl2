// SPDX-License-Identifier: GPL-2.0

#include "gmsl-core.h"
#include "gmsl-max96717.h"

#define MAX96717_I2C_ADDR		0x40
#define MAX96717_ALT_I2C_ADDR	0x62
#define MAX96717_DEV_ID			0xbf
#define MAX96717F_DEV_ID		0xc8

static int max96717_probe(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	u8 id = 0;
	u8 src = 0;
	int ret;

	ret = gmsl_read_addr(gmsl, MAX96717_I2C_ADDR, 0x000d, &id, true);
	if (ret == 0 && (id == MAX96717_DEV_ID || id == MAX96717F_DEV_ID)) {
		src = MAX96717_I2C_ADDR;
	} else {
		ret = gmsl_read_addr(gmsl, MAX96717_ALT_I2C_ADDR, 0x000d,
				     &id, true);
		if (ret == 0 &&
		    (id == MAX96717_DEV_ID || id == MAX96717F_DEV_ID))
			src = MAX96717_ALT_I2C_ADDR;
	}

	if (src) {
		gmsl_write_addr(gmsl, src, 0x0000,
				(MAX96717_I2C_ADDR + link->index + 1) << 1);
		mdelay(75);
	}

	link->ser_addr = MAX96717_I2C_ADDR + link->index + 1;
	mdelay(75);

	ret = gmsl_read_addr(gmsl, link->ser_addr, 0x000d, &id, true);
	if (ret && src) {
		link->ser_addr = src;
		ret = gmsl_read_addr(gmsl, link->ser_addr, 0x000d, &id, false);
	}
	if (ret || (id != MAX96717_DEV_ID && id != MAX96717F_DEV_ID)) {
		dev_err(gmsl->dev,
			"max96724-imx390: serializer NOT detected (max96717)\n");
		return -ENODEV;
	}

	dev_info(gmsl->dev, "max96724-imx390: serializer detected (max96717) at 0x%02x\n",
		 link->ser_addr);
	return 0;
}

static int max96717_configure(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	u8 addr = link->ser_addr;
	u8 dt = link->desc->csi_dt;
	u8 id = 0x30 + link->index;

	gmsl_write_addr(gmsl, addr, 0x02be, 0x10);
	gmsl_write_addr(gmsl, addr, 0x02bf, 0x60);
	gmsl_write_addr(gmsl, addr, 0x0314, 0x40 | dt);
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

const struct gmsl_ser_ops max96717_ser_ops = {
	.name = "max96717",
	.id = MAX96717_DEV_ID,
	.probe = max96717_probe,
	.configure = max96717_configure,
	.gpio_frame_sync = gmsl_gpio_frame_sync,
};
