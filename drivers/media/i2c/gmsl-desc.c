// SPDX-License-Identifier: GPL-2.0

#include <media/v4l2-mediabus.h>

#include "gmsl-core.h"
#include "gmsl-desc.h"
#include "gmsl-imx390.h"
#include "gmsl-max9295.h"

const struct gmsl_sensor_desc imx390_desc = {
	.compatible = "zuojisi,max96724-imx390",
	.name = "imx390",
	.width = 1920,
	.height = 1080,
	.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
	.csi_dt = 0x1e,
	.num_lanes = 4,
	.link_freq = 456000000,
	.ser_ops = &max9295_ser_ops,
	.sensor_ops = &imx390_sensor_ops,
	.init = NULL,
	.init_len = 0,
	.start = NULL,
	.start_len = 0,
	.stop = NULL,
	.stop_len = 0,
};
