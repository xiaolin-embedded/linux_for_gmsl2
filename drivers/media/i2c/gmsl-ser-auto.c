// SPDX-License-Identifier: GPL-2.0

#include "gmsl-core.h"
#include "gmsl-max9295.h"
#include "gmsl-max96717.h"
#include "gmsl-ser-auto.h"

#define GMSL_SER_DEFAULT_ADDR	0x40

static int gmsl_ser_auto_select(struct gmsl_link *link,
				const struct gmsl_ser_ops *ops)
{
	int ret = ops->probe(link);

	if (!ret)
		link->ser_ops = ops;

	return ret;
}

static int gmsl_ser_auto_pick(struct gmsl_link *link, u8 id)
{
	if (id == max9295_ser_ops.id)
		return gmsl_ser_auto_select(link, &max9295_ser_ops);
	if (id == 0xbf || id == 0xc8)
		return gmsl_ser_auto_select(link, &max96717_ser_ops);

	return -ENODEV;
}

static int gmsl_ser_auto_probe(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	u8 id = 0;
	int ret;

	ret = gmsl_read_addr(gmsl, GMSL_SER_DEFAULT_ADDR, 0x000d, &id, true);
	if (!ret) {
		ret = gmsl_ser_auto_pick(link, id);
		if (!ret)
			return 0;
	}

	/* The serializer may already have been moved to 0x41 + link. */
	ret = gmsl_read_addr(gmsl, GMSL_SER_DEFAULT_ADDR + link->index + 1,
			     0x000d, &id, true);
	if (!ret) {
		ret = gmsl_ser_auto_pick(link, id);
		if (!ret)
			return 0;
	}

	ret = gmsl_ser_auto_select(link, &max9295_ser_ops);
	if (!ret)
		return 0;

	return gmsl_ser_auto_select(link, &max96717_ser_ops);
}

static int gmsl_ser_auto_configure(struct gmsl_link *link)
{
	return -EINVAL;
}

const struct gmsl_ser_ops gmsl_ser_auto_ops = {
	.name = "max9295/max96717",
	.probe = gmsl_ser_auto_probe,
	.configure = gmsl_ser_auto_configure,
};
