// SPDX-License-Identifier: GPL-2.0

#include <linux/array_size.h>
#include <media/v4l2-mediabus.h>

#include "gmsl-ar0820.h"
#include "gmsl-core.h"
#include "gmsl-desc.h"
#include "gmsl-imx390.h"
#include "gmsl-imx490.h"
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
	.compatible = "zuojisi,max96724-imx390",
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
	.compatible = "zuojisi,max96724-ar0820",
	.des_ops = &max96724_des_ops,
	.num_links = ARRAY_SIZE(max96724_ar0820_links),
	.links = max96724_ar0820_links,
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
	.compatible = "zuojisi,max96724-imx490",
	.des_ops = &max96724_des_ops,
	.num_links = ARRAY_SIZE(max96724_imx490_links),
	.links = max96724_imx490_links,
	.phy_clk = 0x32,
};
