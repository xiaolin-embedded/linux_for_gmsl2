// SPDX-License-Identifier: GPL-2.0

#ifndef __GMSL_CORE_H__
#define __GMSL_CORE_H__

#include <linux/delay.h>
#include <linux/i2c.h>
#include <media/v4l2-common.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define GMSL_MAX_LINKS 4

struct gmsl_dev;
struct gmsl_link;

struct gmsl_reg_value {
	u16 reg_addr;
	u8 val;
	u8 mask;
	u32 delay_ms;
};

struct gmsl_des_ops {
	const char *name;
	int (*init)(struct gmsl_dev *gmsl);
	int (*configure_link)(struct gmsl_dev *gmsl, struct gmsl_link *link);
	int (*monopolize_link)(struct gmsl_dev *gmsl, int link);
	int (*restore_link)(struct gmsl_dev *gmsl);
	int (*enable_pipes)(struct gmsl_dev *gmsl, u8 link_mask, bool on);
};

struct gmsl_ser_ops {
	const char *name;
	u8 id;
	int (*probe)(struct gmsl_link *link);
	int (*configure)(struct gmsl_link *link);
	int (*gpio_frame_sync)(struct gmsl_link *link);
};

struct gmsl_sensor_ops {
	const char *name;
	int (*init)(struct gmsl_link *link);
	int (*start_stream)(struct gmsl_link *link, bool on);
	int (*get_fmt)(struct gmsl_link *link, struct v4l2_mbus_framefmt *fmt);
	int (*get_mbus_config)(struct gmsl_link *link,
			       struct v4l2_mbus_config *cfg);
};

struct gmsl_sensor_desc {
	const char *compatible;
	const char *name;
	u32 width;
	u32 height;
	u32 mbus_code;
	u8 csi_dt;
	const struct gmsl_sensor_ops *sensor_ops;
	const struct gmsl_reg_value *init;
	unsigned int init_len;
	const struct gmsl_reg_value *start;
	unsigned int start_len;
	const struct gmsl_reg_value *stop;
	unsigned int stop_len;
};

struct gmsl_link_desc {
	unsigned int index;
	const struct gmsl_sensor_desc *sensor;
	const struct gmsl_ser_ops *ser_ops;
	u8 sensor_addr;
	u8 num_lanes;
	u64 link_freq;
};

struct gmsl_board_desc {
	const char *compatible;
	const struct gmsl_des_ops *des_ops;
	unsigned int num_links;
	const struct gmsl_link_desc *links;
	u8 phy_clk;
};

struct gmsl_link {
	struct gmsl_dev *dev;
	unsigned int index;
	const struct gmsl_sensor_desc *desc;
	const struct gmsl_ser_ops *ser_ops;
	struct v4l2_mbus_framefmt fmt;
	u8 ser_addr;
	u8 sensor_addr;
	u8 num_lanes;
	u64 link_freq;
	bool locked;
};

struct gmsl_dev {
	struct i2c_client *client;
	struct device *dev;
	const struct gmsl_board_desc *board;
	const struct gmsl_des_ops *des_ops;
	struct gmsl_link links[GMSL_MAX_LINKS];
	struct v4l2_mbus_config mbus_config;
	u8 dt_addr;
	u8 link_lock_status;
	u8 streaming_mask;
};

int gmsl_write_addr(struct gmsl_dev *gmsl, u8 addr, u16 reg, u8 val);
int gmsl_read_addr(struct gmsl_dev *gmsl, u8 addr, u16 reg, u8 *val,
		   bool quiet);
int gmsl_mod_reg_addr(struct gmsl_dev *gmsl, u8 addr, u16 reg, u8 mask,
		      u8 val);
int gmsl_load_regs(struct gmsl_dev *gmsl, u8 addr,
		   const struct gmsl_reg_value *regs, unsigned int regnum);
int gmsl_gpio_frame_sync(struct gmsl_link *link);

int gmsl_core_set_board(struct gmsl_dev *gmsl,
			const struct gmsl_board_desc *board);
int gmsl_core_hw_init(struct gmsl_dev *gmsl);
int gmsl_core_sensor_init(struct gmsl_dev *gmsl);
int gmsl_core_s_stream(struct gmsl_dev *gmsl, u8 link_mask, bool enable);

#endif
