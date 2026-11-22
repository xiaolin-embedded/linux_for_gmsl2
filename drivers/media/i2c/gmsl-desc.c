// SPDX-License-Identifier: GPL-2.0

#include <linux/array_size.h>
#include <media/v4l2-mediabus.h>

#include "gmsl-ar0820.h"
#include "gmsl-core.h"
#include "gmsl-desc.h"
#include "gmsl-imx390.h"
#include "gmsl-imx490.h"
#include "gmsl-isx031.h"
#include "gmsl-max9295.h"
#include "gmsl-max96724.h"
#include "gmsl-ser-auto.h"

/*
 * DT binding placeholder.  The IMX390 is at 0x21 but sits behind the GW5200
 * ISP on SG2-IMX390C, so it is not directly visible on the serializer bus.
 */
#define IMX390_SENSOR_ADDR	0x36

/*
 * DT binding placeholder.  The AR0820 is at 0x10 but sits behind the GW5300
 * ISP on SG8S-AR0820C, so it is not directly visible on the serializer bus.
 */
#define AR0820_SENSOR_ADDR	0x36

/* DT binding placeholder; IMX490 is behind the module ISP. */
#define IMX490_SENSOR_ADDR	0x36

/* ISX031 is directly visible on the serializer pass-through bus. */
#define ISX031_SENSOR_ADDR	0x1a

/* SG-2M-120A-820-5300-G2 is behind the module ISP, like AR0820. */
#define SG2M_820_SENSOR_ADDR	0x36
#define GMSL_DES_ADDR		0x27

static const struct gmsl_sensor_desc imx390_desc = {
	.compatible = "sony,imx390",
	.name = "imx390",
	.width = 1920,
	.height = 1080,
	.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
	.csi_dt = 0x1e,
	.sensor_ops = &imx390_sensor_ops,
	.init = NULL,
	.init_len = 0,
	.start = NULL,
	.start_len = 0,
	.stop = NULL,
	.stop_len = 0,
};

static const struct gmsl_sensor_desc ar0820_desc = {
	.compatible = "onsemi,ar0820",
	.name = "ar0820",
	.width = 3840,
	.height = 2160,
	.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
	.csi_dt = 0x1e,
	.sensor_ops = &ar0820_sensor_ops,
	.init = NULL,
	.init_len = 0,
	.start = NULL,
	.start_len = 0,
	.stop = NULL,
	.stop_len = 0,
};

static const struct gmsl_sensor_desc sg2m_820_desc = {
	.compatible = "sensing,sg2m-820",
	.name = "sg2m-820",
	.width = 1920,
	.height = 1080,
	.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
	.csi_dt = 0x1e,
	.sensor_ops = &ar0820_sensor_ops,
};

static const struct gmsl_sensor_desc imx490_desc = {
	.compatible = "sony,imx490",
	.name = "imx490",
	.width = 2880,
	.height = 1860,
	.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
	.csi_dt = 0x1e,
	.sensor_ops = &imx490_sensor_ops,
	.init = NULL,
	.init_len = 0,
	.start = NULL,
	.start_len = 0,
	.stop = NULL,
	.stop_len = 0,
};

static const struct gmsl_sensor_desc isx031_desc = {
	.compatible = "sony,isx031",
	.name = "isx031",
	.width = 1920,
	.height = 1536,
	.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
	.csi_dt = 0x1e,
	.sensor_ops = &isx031_sensor_ops,
};

static int gmsl_internal_fsync_init(struct gmsl_link *link);

static const struct gmsl_link_desc max96724_imx390_links[] = {
	{
		.index = 0,
		.sensor = &imx390_desc,
		.ser_ops = &max9295_ser_ops,
		.sensor_addr = IMX390_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 456000000,
	},
};

const struct gmsl_board_desc max96724_imx390_board = {
	.compatible = "xiaolin,max96724-imx390",
	.des_ops = &max96724_des_ops,
	.num_links = ARRAY_SIZE(max96724_imx390_links),
	.links = max96724_imx390_links,
};

static const struct gmsl_link_desc max96724_ar0820_links[] = {
	{
		.index = 0,
		.sensor = &ar0820_desc,
		.ser_ops = &gmsl_ser_auto_ops,
		.sensor_addr = AR0820_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 456000000,
	},
};

const struct gmsl_board_desc max96724_ar0820_board = {
	.compatible = "xiaolin,max96724-ar0820",
	.des_ops = &max96724_des_ops,
	.num_links = ARRAY_SIZE(max96724_ar0820_links),
	.links = max96724_ar0820_links,
	.skip_frame_sync = true,
	.fsync_init = gmsl_internal_fsync_init,
};

static const struct gmsl_link_desc max96724_imx490_links[] = {
	{
		.index = 0,
		.sensor = &imx490_desc,
		.ser_ops = &gmsl_ser_auto_ops,
		.sensor_addr = IMX490_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 456000000,
	},
	{
		.index = 1,
		.sensor = &imx490_desc,
		.ser_ops = &gmsl_ser_auto_ops,
		.sensor_addr = IMX490_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 456000000,
	},
	{
		.index = 2,
		.sensor = &imx490_desc,
		.ser_ops = &gmsl_ser_auto_ops,
		.sensor_addr = IMX490_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 456000000,
	},
	{
		.index = 3,
		.sensor = &imx490_desc,
		.ser_ops = &gmsl_ser_auto_ops,
		.sensor_addr = IMX490_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 456000000,
	},
};

const struct gmsl_board_desc max96724_imx490_board = {
	.compatible = "xiaolin,max96724-imx490",
	.des_ops = &max96724_des_ops,
	.num_links = ARRAY_SIZE(max96724_imx490_links),
	.links = max96724_imx490_links,
	.phy_clk = 0x32,
};

static const struct gmsl_link_desc max96724_isx031_links[] = {
	{
		.index = 0,
		.sensor = &isx031_desc,
		.ser_ops = &gmsl_ser_auto_ops,
		.sensor_addr = ISX031_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 288000000,
	},
};

const struct gmsl_board_desc max96724_isx031_board = {
	.compatible = "xiaolin,max96724-isx031",
	.des_ops = &max96724_des_ops,
	.num_links = ARRAY_SIZE(max96724_isx031_links),
	.links = max96724_isx031_links,
	.phy_clk = 0x25,
	.three_gbps = true,
	.skip_frame_sync = true,
};

static const struct gmsl_link_desc max96724_sg2m820_links[] = {
	{
		.index = 0,
		.sensor = &sg2m_820_desc,
		.ser_ops = &gmsl_ser_auto_ops,
		.sensor_addr = SG2M_820_SENSOR_ADDR,
		.num_lanes = 4,
		.link_freq = 456000000,
	},
};

static const struct gmsl_reg_value gmsl_internal_fsync_des[] = {
	{ 0x04a0, 0x00 },
	{ 0x04a2, 0x00 },
	{ 0x04a5, 0x35 },
	{ 0x04a6, 0xb7 },
	{ 0x04a7, 0x0c },
	{ 0x04af, 0xcf },
	{ 0x04b1, 0x40 },
};

static const struct gmsl_reg_value gmsl_internal_fsync_ser[] = {
	{ 0x02d6, 0x84 },
	{ 0x02d7, 0xa0 },
	{ 0x02d8, 0x08 },
};

static int gmsl_internal_fsync_init(struct gmsl_link *link)
{
	struct gmsl_dev *gmsl = link->dev;
	int ret;

	ret = gmsl_load_regs(gmsl, GMSL_DES_ADDR, gmsl_internal_fsync_des,
			     ARRAY_SIZE(gmsl_internal_fsync_des));
	if (ret)
		return ret;

	ret = gmsl_write_addr(gmsl, GMSL_DES_ADDR,
			      0x0b08 + link->index * 0x100, 0x10);
	if (ret)
		return ret;

	ret = gmsl_load_regs(gmsl, link->ser_addr, gmsl_internal_fsync_ser,
			     ARRAY_SIZE(gmsl_internal_fsync_ser));
	if (ret)
		return ret;

	mdelay(20);
	return 0;
}

const struct gmsl_board_desc max96724_sg2m820_board = {
	.compatible = "xiaolin,max96724-sg2m820",
	.des_ops = &max96724_des_ops,
	.num_links = ARRAY_SIZE(max96724_sg2m820_links),
	.links = max96724_sg2m820_links,
	.skip_frame_sync = true,
	.fsync_init = gmsl_internal_fsync_init,
};
