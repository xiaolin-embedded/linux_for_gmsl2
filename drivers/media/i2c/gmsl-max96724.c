// SPDX-License-Identifier: GPL-2.0

#include <linux/bitops.h>

#include "gmsl-core.h"
#include "gmsl-max96724.h"

#define MAX96724_ADDR				0x27
#define MAX96724_PHY_CLK			(0x20 | 16)
#define MAX96724_TX11_PIPE_X_EN_ADDR		0x90b
#define MAX96724_TX45_PIPE_X_DST_CTRL_ADDR	0x92d
#define MAX96724_PIPE_X_SRC_0_MAP_ADDR		0x90d

static int max96724_write(struct gmsl_dev *gmsl, u16 reg, u8 val)
{
	return gmsl_write_addr(gmsl, MAX96724_ADDR, reg, val);
}

static int max96724_read(struct gmsl_dev *gmsl, u16 reg, u8 *val)
{
	return gmsl_read_addr(gmsl, MAX96724_ADDR, reg, val, false);
}

static int max96724_check_version(struct gmsl_dev *gmsl)
{
	static const u16 veraddrs[] = { 0x0316, 0x0319 };
	u8 ver = 0, tmp = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(veraddrs); i++) {
		max96724_read(gmsl, veraddrs[i], &tmp);
		tmp = (tmp & 0x8) >> 3;
		ver |= tmp << i;
	}

	return ver == 0x01 ? 0 : -1;
}

static int max96724_get_link_lock(struct gmsl_dev *gmsl)
{
	u8 val = 0;
	u8 mask = 0;

	max96724_read(gmsl, 0x001a, &val);
	mask |= (val & 0x08) >> 3;
	max96724_read(gmsl, 0x000a, &val);
	mask |= (val & 0x08) >> 2;
	max96724_read(gmsl, 0x000b, &val);
	mask |= (val & 0x08) >> 1;
	max96724_read(gmsl, 0x000c, &val);
	mask |= (val & 0x08) >> 0;

	gmsl->link_lock_status = mask;
	return 0;
}

static int max96724_init(struct gmsl_dev *gmsl)
{
	struct device *dev = gmsl->dev;
	u8 val = 0;
	u8 phy_clk;

	max96724_read(gmsl, 0x0000, &val);
	if (val != 0x4e) {
		dev_err(dev, "max96724-imx390: deserializer NOT detected\n");
		return -ENODEV;
	}
	dev_info(dev, "max96724-imx390: deserializer detected\n");

	if (max96724_check_version(gmsl)) {
		dev_err(dev, "max96724-imx390: unsupported deserializer version\n");
		return -EINVAL;
	}

	max96724_write(gmsl, 0x0013, 0x40); /* reset all */
	mdelay(100);
	max96724_write(gmsl, 0x0013, 0x00);

	max96724_write(gmsl, 0x0005, 0x88);
	max96724_write(gmsl, 0x0310, 0x90);
	max96724_write(gmsl, 0x0309, 0x90); /* turn on LED */

	if (gmsl->board && gmsl->board->three_gbps) {
		max96724_write(gmsl, 0x0010, 0x11);
		max96724_write(gmsl, 0x0011, 0x11);
		max96724_write(gmsl, 0x0006, 0xff);
		max96724_write(gmsl, 0x0018, 0x0f);
		mdelay(150);
	}

	max96724_get_link_lock(gmsl);

	/* Disable links which are not locked. */
	max96724_read(gmsl, 0x0006, &val);
	max96724_write(gmsl, 0x0006,
		       (val & 0xf0) | (gmsl->link_lock_status & 0x0f));

	dev_info(dev, "max96724-imx390: link lock status=0x%02x\n",
		 gmsl->link_lock_status);

	if (!gmsl->link_lock_status) {
		dev_err(dev, "max96724-imx390: no camera detected\n");
		return -EINVAL;
	}

	/* 4 lanes, D-PHY, 2-bit VC */
	max96724_write(gmsl, 0x90a, 0xc0);
	max96724_write(gmsl, 0x94a, 0xc0);
	max96724_write(gmsl, 0x98a, 0xc0);
	max96724_write(gmsl, 0x9ca, 0xc0);

	/* PHY lane mapping and 2x4 mode */
	max96724_write(gmsl, 0x8a3, 0xe4);
	max96724_write(gmsl, 0x8a4, 0xe4);
	max96724_write(gmsl, 0x8a0, 0x24);

	phy_clk = gmsl->board && gmsl->board->phy_clk ?
			  gmsl->board->phy_clk : MAX96724_PHY_CLK;
	max96724_write(gmsl, 0x415, phy_clk);
	max96724_write(gmsl, 0x418, phy_clk);
	max96724_write(gmsl, 0x41b, phy_clk);
	max96724_write(gmsl, 0x41e, phy_clk);

	/* pipe1 GMSL-B, Z-pipe; pipe0 GMSL-A, Z-pipe */
	max96724_write(gmsl, 0xf0, 0x62);
	/* pipe3 GMSL-D, Z-pipe; pipe2 GMSL-C, Z-pipe */
	max96724_write(gmsl, 0xf1, 0xea);
	max96724_write(gmsl, 0xf4, 0x00);

	max96724_write(gmsl, 0x0300, 0x83); /* TX on A */
	max96724_write(gmsl, 0x0301, 0xaa); /* TX on A */
	max96724_write(gmsl, 0x0337, 0x2a); /* TX on B */
	max96724_write(gmsl, 0x036d, 0x2a); /* TX on C */
	max96724_write(gmsl, 0x03a4, 0x2a); /* TX on D */

	max96724_write(gmsl, 0x0027, 0x00);
	max96724_write(gmsl, 0x0029, 0xf0);

	return 0;
}

static int max96724_configure_link(struct gmsl_dev *gmsl,
				   struct gmsl_link *link)
{
	unsigned int i = link->index;
	u8 dt = link->desc->csi_dt;

	max96724_write(gmsl, (0x40 * i) + MAX96724_TX11_PIPE_X_EN_ADDR, 0x1f);
	/* 0x55 = controller1, 0xaa = controller2 */
	max96724_write(gmsl, (0x40 * i) + MAX96724_TX45_PIPE_X_DST_CTRL_ADDR,
		       0xaa);
	max96724_write(gmsl, (0x40 * i) + MAX96724_TX45_PIPE_X_DST_CTRL_ADDR + 1,
		       0x02);
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 0,
		       dt);
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 1,
		       dt | (i << 6));
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 2,
		       0x00);
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 3,
		       0x00 | (i << 6));
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 4,
		       0x01);
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 5,
		       0x01 | (i << 6));
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 6,
		       0x02);
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 7,
		       0x02 | (i << 6));
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 8,
		       0x03);
	max96724_write(gmsl, (0x40 * i) + MAX96724_PIPE_X_SRC_0_MAP_ADDR + 9,
		       0x03 | (i << 6));

	return 0;
}

static int max96724_monopolize_link(struct gmsl_dev *gmsl, int link)
{
	u8 value = 0;

	dev_info(gmsl->dev,
		 "max96724-imx390: %s: monopolize link_%c\n",
		 __func__, 'A' + link);

	max96724_read(gmsl, 0x0006, &value);
	value = (value & 0xf0) | BIT(link);
	max96724_write(gmsl, 0x0006, value);
	mdelay(150);

	return 0;
}

static int max96724_restore_link(struct gmsl_dev *gmsl)
{
	max96724_write(gmsl, 0x0006, 0xf0 | gmsl->link_lock_status);
	mdelay(150);

	return 0;
}

static int max96724_enable_pipes(struct gmsl_dev *gmsl, u8 link_mask, bool on)
{
	/*
	 * Keep every locked link's pipe enabled. Disabling one link's pipe
	 * also stops its remote ISP output, which cannot be recovered by
	 * simply re-enabling the pipe later.
	 */
	if (!on)
		return 0;

	max96724_write(gmsl, 0x00f4, link_mask);
	max96724_write(gmsl, 0x0018, link_mask);

	if (!link_mask)
		return 0;

	max96724_write(gmsl, 0x0943, 0x10);
	max96724_write(gmsl, 0x0943, 0x33);
	max96724_write(gmsl, 0x0983, 0x10);
	max96724_write(gmsl, 0x0983, 0x33);

	mdelay(100);

	dev_dbg(gmsl->dev, "max96724-imx390: link lock status =%x\n",
		gmsl->link_lock_status);

	return 0;
}

const struct gmsl_des_ops max96724_des_ops = {
	.name = "max96724",
	.init = max96724_init,
	.configure_link = max96724_configure_link,
	.monopolize_link = max96724_monopolize_link,
	.restore_link = max96724_restore_link,
	.enable_pipes = max96724_enable_pipes,
};
