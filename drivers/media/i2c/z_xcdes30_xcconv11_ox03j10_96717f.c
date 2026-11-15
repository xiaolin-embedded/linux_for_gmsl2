#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define MAX96712_PHY_CLK (0x20|16)
#define MAX96712_TX11_PIPE_X_EN_ADDR 0x90B
#define MAX96712_TX45_PIPE_X_DST_CTRL_ADDR 0x92D
#define MAX96712_PIPE_X_SRC_0_MAP_ADDR 0x90D

#define CAM_DEF_ADDR (0x36)
#define CAM_TRANS_ADDR_BASE (0x14)
#define MAX96717_DEF_ADDR (0x42)
#define MAX96717_TRANS_ADDR_BASE (0x48)

#define CAM_WIDTH (1920)
#define CAME_HEIGHT (1536)

#define 	FMT_CODE (MEDIA_BUS_FMT_UYVY8_1X16)

/* regulator supplies */
static const char * const ox03j10_96717f_supply_name[] = {
	"DOVDD", /* Digital I/O (1.8V) supply */
	"AVDD",  /* Analog (2.8V) supply */
	"DVDD",  /* Digital Core (1.5V) supply */
};

#define OX03J10_96717F_NUM_SUPPLIES ARRAY_SIZE(ox03j10_96717f_supply_name)

struct reg_value {
	u16 reg_addr;
	u8 val;
	u8 mask;
	u32 delay_ms;
};

struct ox03j10_96717f_dev {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad[4];
	struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */
	struct clk *xclk; /* system clock to OX03J10_96717F */
	u32 xclk_freq;

	struct regulator_bulk_data supplies[OX03J10_96717F_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	bool   upside_down;

	/* lock to protect all members below */
	struct mutex lock;
	bool streaming;
	u8 link_lock_status;
};

static inline struct ox03j10_96717f_dev *to_ox03j10_96717f_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ox03j10_96717f_dev, sd);
}

static struct v4l2_mbus_framefmt ox03j10_96717f_default_fmt = {
	.code = FMT_CODE,
	.width = CAM_WIDTH,
	.height = CAME_HEIGHT,
	.colorspace = V4L2_COLORSPACE_REC709,
	.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_REC709),
	.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(V4L2_COLORSPACE_REC709),
	.field = V4L2_FIELD_NONE,
};

static const struct reg_value ox03j10_96717f_init_setting[] = {    
	{0x2407, 0x00, 0, 10},
};

static const struct reg_value ox03j10_96717f_start_stream[] = {
	{0x0100, 0x01, 0, 10}, //WAKE UP
};

static const struct reg_value ox03j10_96717f_stop_stream[] = {
	{0x0100, 0x00, 0, 10} //SW Power Down, wait 10ms
};

static int ox03j10_96717f_write_reg(struct ox03j10_96717f_dev *sensor, u16 reg, u8 val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[3];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "zuojisi: %s: error: slave=%x, reg=%x, val=%x\n",
			__func__, client->addr, reg, val);
		return ret;
	}

	dev_info(&client->dev, "zuojisi: w 0x%02x : (0x%04x)->0x%02x\n",client->addr,reg,val);

	return 0;
}

static int ox03j10_96717f_read_reg(struct ox03j10_96717f_dev *sensor, u16 reg, u8 *val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = 1;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_err(&client->dev, "zuojisi: %s: error: slave=%x reg=%x\n",
			__func__, client->addr, reg);
		return ret;
	}

	*val = buf[0];
	dev_info(&client->dev, "zuojisi: r 0x%02x : (0x%04x)<-0x%02x\n",client->addr,reg, *val);
	return 0;
}

static int ox03j10_96717f_mod_reg(struct ox03j10_96717f_dev *sensor, u16 reg,
			  u8 mask, u8 val)
{
	u8 readval;
	int ret;

	ret = ox03j10_96717f_read_reg(sensor, reg, &readval);
	if (ret)
		return ret;

	readval &= ~mask;
	val &= mask;
	val |= readval;

	return ox03j10_96717f_write_reg(sensor, reg, val);
}


static void ox03j10_96717f_load_regs(struct ox03j10_96717f_dev *sensor,
			     const struct reg_value *regs, unsigned int regnum)
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
			ret = ox03j10_96717f_mod_reg(sensor, reg_addr, mask, val);
		else
			ret = ox03j10_96717f_write_reg(sensor, reg_addr, val);
		if (ret)
			break;

		if (delay_ms)
			usleep_range(1000 * delay_ms, 1000 * delay_ms + 100);
	}
}


/* restore the last set video mode after chip power-on */
static int ox03j10_96717f_init_base_settings(struct ox03j10_96717f_dev *sensor)
{
	/* first load the initial register values */
	ox03j10_96717f_load_regs(sensor, ox03j10_96717f_init_setting, ARRAY_SIZE(ox03j10_96717f_init_setting));

	return 0;
}

static void ox03j10_96717f_power(struct ox03j10_96717f_dev *sensor, bool enable)
{
	gpiod_set_value_cansleep(sensor->pwdn_gpio, enable ? 0 : 1);
}

/*
 * From section 2.7 power up sequence:
 * t0 + t1 + t2 >= 5ms	Delay from DOVDD stable to PWDN pull down
 * t3 >= 1ms		Delay from PWDN pull down to RESETB pull up
 * t4 >= 20ms		Delay from RESETB pull up to SCCB (i2c) stable
 *
 * Some modules don't expose RESETB/PWDN pins directly, instead providing a
 * "PWUP" GPIO which is wired through appropriate delays and inverters to the
 * pins.
 *
 * In such cases, this gpio should be mapped to pwdn_gpio in the driver, and we
 * should still toggle the pwdn_gpio below with the appropriate delays, while
 * the calls to reset_gpio will be ignored.
 */
static void ox03j10_96717f_powerup_sequence(struct ox03j10_96717f_dev *sensor)
{
	if (sensor->pwdn_gpio) {
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);

		/* camera power cycle */
		ox03j10_96717f_power(sensor, false);
		usleep_range(5000, 10000);
		ox03j10_96717f_power(sensor, true);
		usleep_range(5000, 10000);

		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
		usleep_range(1000, 2000);

		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	}
	usleep_range(20000, 25000);

}

static int ox03j10_96717f_set_power_on(struct ox03j10_96717f_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		return ret;
	}

	ret = regulator_bulk_enable(OX03J10_96717F_NUM_SUPPLIES,
				    sensor->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		goto xclk_off;
	}

	ox03j10_96717f_powerup_sequence(sensor);

	return 0;

xclk_off:
	clk_disable_unprepare(sensor->xclk);
	return ret;
}

static void ox03j10_96717f_set_power_off(struct ox03j10_96717f_dev *sensor)
{
	ox03j10_96717f_power(sensor, false);
	regulator_bulk_disable(OX03J10_96717F_NUM_SUPPLIES, sensor->supplies);
	clk_disable_unprepare(sensor->xclk);
}


static int ox03j10_96717f_set_power(struct ox03j10_96717f_dev *sensor, bool on)
{
	int ret = 0;

	if (on) {
		ret = ox03j10_96717f_set_power_on(sensor);
		if (ret)
			return ret;

		ret = ox03j10_96717f_init_base_settings(sensor);
		if (ret)
			goto power_off;
	}

	if (!on)
		ox03j10_96717f_set_power_off(sensor);

	return 0;

power_off:
	ox03j10_96717f_set_power_off(sensor);
	return ret;
}

static int ox03j10_96717f_sensor_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ox03j10_96717f_dev *ox03j10_96717f = to_ox03j10_96717f_dev(sd);

	return ox03j10_96717f_set_power(ox03j10_96717f, false);
}

static int ox03j10_96717f_sensor_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ox03j10_96717f_dev *ox03j10_96717f = to_ox03j10_96717f_dev(sd);

	return ox03j10_96717f_set_power(ox03j10_96717f, true);
}

/* --------------- Subdev Operations --------------- */
static int ox03j10_96717f_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s , pad=%d\n",__func__, format->pad);

	if (format->pad > 3)
		return -EINVAL;

	mutex_lock(&sensor->lock);
	format->format = ox03j10_96717f_default_fmt;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int ox03j10_96717f_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	int ret = 0;
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);
	dev_info(&sensor->i2c_client->dev, "zuojisi: which=%s", format->which ? "V4L2_SUBDEV_FORMAT_ACTIVE":"V4L2_SUBDEV_FORMAT_TRY");
	dev_info(&sensor->i2c_client->dev, "zuojisi: code=%d, colorspace=%d",format->format.code, format->format.colorspace);
	dev_info(&sensor->i2c_client->dev, "zuojisi: xfer_func=%d, ycbcr_enc=%d",format->format.xfer_func, format->format.ycbcr_enc);
	dev_info(&sensor->i2c_client->dev, "zuojisi: quantization=%d, field=%d",format->format.quantization, format->format.field);
	dev_info(&sensor->i2c_client->dev, "zuojisi: width=%d, height=%d",format->format.width, format->format.height);

	if (format->pad > 3)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, 0) = format->format;
		goto out;
	}

	ox03j10_96717f_default_fmt = format->format;

	ret = 0;
out:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int ox03j10_96717f_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		mutex_lock(&sensor->lock);
		sel->r.left = sel->r.top = 0;
		sel->r.width = CAM_WIDTH;
		sel->r.height = CAME_HEIGHT;
		mutex_unlock(&sensor->lock);
		return 0;
	}

	return -EINVAL;
}

static int ox03j10_96717f_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned int index = fse->index;
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	if (fse->pad > 3)
		return -EINVAL;

	if (index != 0)
		return -EINVAL;

	fse->min_width = CAM_WIDTH;
	fse->max_width = fse->min_width;
	fse->min_height = CAME_HEIGHT;
	fse->max_height = fse->min_height;

	return 0;
}

static int ox03j10_96717f_enum_frame_interval(
	struct v4l2_subdev *sd,
	struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval_enum *fie)
{
	struct v4l2_fract tpf;
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s , pad=%d, index=%d\n",__func__, fie->pad,fie->index);

	if (fie->pad > 3)
		return -EINVAL;
	if (fie->index != 0)
		return -EINVAL;

	tpf.numerator = 1;
	tpf.denominator = 30;

	fie->interval = tpf;
	return 0;
}

static int ox03j10_96717f_get_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	mutex_lock(&sensor->lock);
	fi->interval.numerator = 1;
	fi->interval.denominator = 30;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int ox03j10_96717f_set_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	int ret = 0;
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	if (fi->pad > 3)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}

	mutex_unlock(&sensor->lock);
	return 0; //fix 30fps
out:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int ox03j10_96717f_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;

	code->code = FMT_CODE;

	return 0;
}

static int ox03j10_96717f_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	int ret = 0,i;
	u16 addrbackup;

	dev_info(&sensor->i2c_client->dev, "zuojisi: s stream enable=%d\n",enable);

	if (enable) {
		ret = pm_runtime_resume_and_get(&sensor->i2c_client->dev);
		if (ret < 0)
			return ret;
	}

	mutex_lock(&sensor->lock);
	if (sensor->streaming == enable) {
		mutex_unlock(&sensor->lock);
		return 0;
	}

	if (enable) {
		addrbackup = sensor->i2c_client->addr;

		sensor->i2c_client->addr = 0x29;
		ox03j10_96717f_write_reg(sensor,0x00f4, sensor->link_lock_status);
		ox03j10_96717f_write_reg(sensor,0x0018, sensor->link_lock_status);

		ox03j10_96717f_write_reg(sensor,0x0943,0x10);
		ox03j10_96717f_write_reg(sensor,0x0943,0x33);
		ox03j10_96717f_write_reg(sensor,0x0983,0x10);
		ox03j10_96717f_write_reg(sensor,0x0983,0x33);

		mdelay(50);

#define MAX9295_GPIO_FRAME_TRIGGER 0x02D6

		dev_info(&sensor->i2c_client->dev, "zuojisi: link lock status =%x\n",sensor->link_lock_status);

		for(i=0;i<4;i++){
			if(! (sensor->link_lock_status & (0x01<<i) )){
				continue;
			}
			sensor->i2c_client->addr = CAM_TRANS_ADDR_BASE+i;
			ox03j10_96717f_load_regs(sensor, ox03j10_96717f_start_stream, ARRAY_SIZE(ox03j10_96717f_start_stream));

			sensor->i2c_client->addr = MAX96717_TRANS_ADDR_BASE+i;
			ox03j10_96717f_write_reg(sensor, MAX9295_GPIO_FRAME_TRIGGER, 0x80);
			mdelay(50);
			ox03j10_96717f_write_reg(sensor, MAX9295_GPIO_FRAME_TRIGGER, 0x90);
		
			// //receive remote
			// ox03j10_96717f_write_reg(sensor, MAX9295_GPIO_FRAME_TRIGGER, 0x04);
			// ox03j10_96717f_write_reg(sensor, MAX9295_GPIO_FRAME_TRIGGER+1, 0x6A);
			// ox03j10_96717f_write_reg(sensor, MAX9295_GPIO_FRAME_TRIGGER+2, 0x0A);
		}

		sensor->i2c_client->addr = addrbackup;

	} else {
		ox03j10_96717f_load_regs(sensor, ox03j10_96717f_stop_stream, ARRAY_SIZE(ox03j10_96717f_stop_stream));
	}

	sensor->streaming = enable;
	mutex_unlock(&sensor->lock);

	if (!enable || ret)
		pm_runtime_put_autosuspend(&sensor->i2c_client->dev);

	return ret;
}

static int ox03j10_96717f_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt =
				v4l2_subdev_state_get_format(state, 0);
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);

	*fmt = ox03j10_96717f_default_fmt;

	crop->left = 0;
	crop->top = 0;
	crop->width = CAM_WIDTH;
	crop->height = CAME_HEIGHT;

	return 0;
}

static int ox03j10_96717f_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad, struct v4l2_mbus_frame_desc *fd)
{
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].bus.csi2.dt = 0x1e;
	fd->entry[0].bus.csi2.vc = (u8)pad;
	return 0;
}

static const struct v4l2_subdev_core_ops ox03j10_96717f_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops ox03j10_96717f_video_ops = {
	.s_stream = ox03j10_96717f_s_stream,
};

static const struct v4l2_subdev_pad_ops ox03j10_96717f_pad_ops = {
	.enum_mbus_code = ox03j10_96717f_enum_mbus_code,
	.get_fmt = ox03j10_96717f_get_fmt,
	.set_fmt = ox03j10_96717f_set_fmt,
	.get_selection = ox03j10_96717f_get_selection,
	.enum_frame_size = ox03j10_96717f_enum_frame_size,
	.enum_frame_interval = ox03j10_96717f_enum_frame_interval,
	.get_frame_desc = ox03j10_96717f_get_frame_desc,
	.get_frame_interval = ox03j10_96717f_get_frame_interval,
	.set_frame_interval = ox03j10_96717f_set_frame_interval,
};

static const struct v4l2_subdev_internal_ops ox03j10_96717f_internal_ops = {
	.init_state = ox03j10_96717f_init_cfg,
};

static const struct v4l2_subdev_ops ox03j10_96717f_subdev_ops = {
	.core = &ox03j10_96717f_core_ops,
	.video = &ox03j10_96717f_video_ops,
	.pad = &ox03j10_96717f_pad_ops,
};

static int ox03j10_96717f_get_regulators(struct ox03j10_96717f_dev *sensor)
{
	int i;

	for (i = 0; i < OX03J10_96717F_NUM_SUPPLIES; i++)
		sensor->supplies[i].supply = ox03j10_96717f_supply_name[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       OX03J10_96717F_NUM_SUPPLIES,
				       sensor->supplies);
}


static int check_version(struct ox03j10_96717f_dev *sensor)
{
	const uint16_t ver0addr = 0x0326; //ver0
	const uint16_t ver1addr = 0x0310; //ver1
	const uint16_t ver2addr = 0x0313; //ver2
	const uint16_t ver3addr = 0x0316; //ver3

	uint16_t veraddrs[4] = {ver0addr,ver1addr,ver2addr,ver3addr};
	u8 ver=0,tmp,i;

	for(i=0;i<4;i++){
		ox03j10_96717f_read_reg(sensor,veraddrs[i],&tmp);
		tmp = (tmp & 0x8)>>3;
		ver = ver | (tmp<<i);
	}

	return (ver==0x01)?0:-1;
}

static int get_link_lock_status(struct ox03j10_96717f_dev *sensor)
{
	u8 val;
	sensor->link_lock_status = 0;
	ox03j10_96717f_read_reg(sensor,0x001a,&val);
	sensor->link_lock_status |= (val & 0x08)>>3;
	ox03j10_96717f_read_reg(sensor,0x000a,&val);
	sensor->link_lock_status |= (val & 0x08)>>2;
	ox03j10_96717f_read_reg(sensor,0x000b,&val);
	sensor->link_lock_status |= (val & 0x08)>>1;
	ox03j10_96717f_read_reg(sensor,0x000c,&val);
	sensor->link_lock_status |= (val & 0x08)>>0;
	return 0;
}

static int monopolize_link(struct ox03j10_96717f_dev *sensor, int link)
{
	u8 value;
	struct i2c_client *client = sensor->i2c_client;
	struct device *dev = &client->dev;

	dev_info(dev, "zuojisi: %s: monopolize link_%c\n", __func__, 'A'+link);

	ox03j10_96717f_read_reg(sensor,0x0006,&value);
	value = (value & 0xF0) | (1<<link);
	ox03j10_96717f_write_reg(sensor,0x0006,value);
	mdelay(150);
	return 0;
}

static int restore_link(struct ox03j10_96717f_dev *sensor)
{
	ox03j10_96717f_write_reg(sensor,0x0006,0xF0 | sensor->link_lock_status);
	mdelay(150);
	return 0;
}

static int ox03j10_96717f_setup_deserializer(struct ox03j10_96717f_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	struct device *dev = &client->dev;
	u16 addrbackup = client->addr;
	u8 val = 0;
	int ret,i;
	struct i2c_msg msgs[2];
    u8 cmd[3];

	//9546 enable channel 1
	cmd[0] = 0x02;
	msgs[0].addr = 0x70;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = cmd;
	ret = i2c_transfer(client->adapter, msgs, 1);

	//trans 96712-slave to 0x2a
	client->addr = 0x29;
	ox03j10_96717f_write_reg(sensor,0x0000,0x2a<<1);
	mdelay(30);

	//reset 96712-slave
	client->addr = 0x2a;
	ox03j10_96717f_write_reg(sensor,0x0013,0xff);
	msleep(100);

	//trans 96712-slave to 0x2a
	client->addr = 0x29;
	ox03j10_96717f_write_reg(sensor,0x0000,0x2a<<1);
	mdelay(30);

	//9546 enable chanell 0 & 1
	cmd[0] = 0x03;
	msgs[0].addr = 0x70;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = cmd;
	ret = i2c_transfer(client->adapter, msgs, 1);

	//reset 96712-slave
	client->addr = 0x29;
	ox03j10_96717f_write_reg(sensor,0x0013,0xff);
	mdelay(100);


	client->addr = 0x29;
	ox03j10_96717f_write_reg(sensor,0x0013,0x00);

	ret = ox03j10_96717f_read_reg(sensor, 0x0000, &val);
	if(ret == 0 && ( (val == 0x52) || (val == 0x54) ) ){
		dev_info(dev, "zuojisi: deserializer detected\n");
	}else{
		dev_err(dev, "zuojisi: deserializer NOT detected\n");
		goto error;
	}

	if(check_version(sensor)){
		return -EINVAL;
	}

	//enable LDO
    ox03j10_96717f_write_reg(sensor,0x0017,0x04);
    ox03j10_96717f_write_reg(sensor,0x0019,0x10);
 
    ox03j10_96717f_write_reg(sensor,0x0005,0x80);
    ox03j10_96717f_write_reg(sensor,0x0309,0x90);

	//turn on LED
    ox03j10_96717f_write_reg(sensor,0x0300,0x90);

	ox03j10_96717f_write_reg(sensor,0x0010,0x11);
	ox03j10_96717f_write_reg(sensor,0x0011,0x11);
	mdelay(200);

	get_link_lock_status(sensor);
	//disable Links which are not Locked
	ox03j10_96717f_read_reg(sensor,0x0006,&val);
	ox03j10_96717f_write_reg(sensor,0x0006,(val&0xF0)|(sensor->link_lock_status&0x0F));

	dev_err(dev, "zuojisi: link lock status=0x%02x\n",sensor->link_lock_status);
	if(sensor->link_lock_status == 0){
		dev_err(dev, "zuojisi: no camera detected\n");
		return -EINVAL;
	}

	ox03j10_96717f_write_reg(sensor,0x90a,0xc0); //4 lanes, D-Phy, 2bits VC
	ox03j10_96717f_write_reg(sensor,0x94a,0xc0); //4 lanes, D-Phy, 2bits VC
	ox03j10_96717f_write_reg(sensor,0x98a,0xc0); //4 lanes, D-Phy, 2bits VC
	ox03j10_96717f_write_reg(sensor,0x9ca,0xc0); //4 lanes, D-Phy, 2bits VC

	if( client->addr == 0x29 ){
		ox03j10_96717f_write_reg(sensor,0x8a3,0x4e); //phy lane mapping
		ox03j10_96717f_write_reg(sensor,0x8a4,0x4e); //phy lane mapping
	}else{
		ox03j10_96717f_write_reg(sensor,0x8a3,0xe4); //phy lane mapping
		ox03j10_96717f_write_reg(sensor,0x8a4,0xe4); //phy lane mapping
	}

    ox03j10_96717f_write_reg(sensor,0x8a0,0x04); // 2x4 mode

	ox03j10_96717f_write_reg(sensor,0x415,MAX96712_PHY_CLK);
	ox03j10_96717f_write_reg(sensor,0x418,MAX96712_PHY_CLK);
	ox03j10_96717f_write_reg(sensor,0x41b,MAX96712_PHY_CLK);
	ox03j10_96717f_write_reg(sensor,0x41e,MAX96712_PHY_CLK);

	ox03j10_96717f_write_reg(sensor,0xf0,0x62); //pipe1 GMSLB, Z-pipe; pipe0 GMSLA, Z-pipe
	ox03j10_96717f_write_reg(sensor,0xf1,0xea); //pipe3 GMSLD, Z-pipe; pipe2 GMSLC, Z-pipe
	ox03j10_96717f_write_reg(sensor,0xf4,0x00); //disable pipes 0-4 (default val)

#if 0
    //mfp10, enable TX with ID=A
    ox03j10_96717f_write_reg(sensor,0x0320,0x83); //TX on A
    ox03j10_96717f_write_reg(sensor,0x0357,0x2a); //TX on B
    ox03j10_96717f_write_reg(sensor,0x038d,0x2a); //TX on C
    ox03j10_96717f_write_reg(sensor,0x03c4,0x2a); //TX on D
#endif

	ox03j10_96717f_write_reg(sensor,0x0027,0x00); //disable unconcerned error report
	ox03j10_96717f_write_reg(sensor,0x0029,0xf0); //disable unconcerned error report
	ox03j10_96717f_write_reg(sensor,0x0023,0x0f); //auto reset dec error
	ox03j10_96717f_write_reg(sensor,0x1250,0xff); //set 128 ECC threshold, reset error cnt

	for(i=0;i<4;i++){
		if(! (sensor->link_lock_status & (0x01<<i) )){
			continue;
		}
		client->addr = 0x29;

		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_TX11_PIPE_X_EN_ADDR+0, 0x1f);
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_TX45_PIPE_X_DST_CTRL_ADDR+0, 0x55);  // 0x55=controller1  0xAA=controller2
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_TX45_PIPE_X_DST_CTRL_ADDR+1, 0x01); 
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+0, 0x1e);
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+1, 0x1e | (i << 6));
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+2, 0x00);
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+3, 0x00 | (i << 6));
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+4, 0x01);
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+5, 0x01 | (i << 6));
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+6, 0x02);
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+7, 0x02 | (i << 6));
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+8, 0x03);
		ox03j10_96717f_write_reg(sensor, (0x40*i)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+9, 0x03 | (i << 6));

		monopolize_link(sensor,i);

		client->addr = MAX96717_DEF_ADDR;
		ox03j10_96717f_write_reg(sensor, 0x0000, (MAX96717_TRANS_ADDR_BASE+i)<<1);
		client->addr = MAX96717_TRANS_ADDR_BASE+i;
		mdelay(75);

		ret = ox03j10_96717f_read_reg(sensor, 0x000d, &val);
		if(ret == 0 && val == 0xc8 ){
			dev_info(dev, "zuojisi: serializer detected\n");
		}else{
			dev_err(dev, "zuojisi: serializer NOT detected\n");
			sensor->link_lock_status &= ~(1<<i);
			goto error;
		}

		ox03j10_96717f_write_reg(sensor, 0x02bf, 0x60);
		ox03j10_96717f_write_reg(sensor, 0x02be, 0x90);

		ox03j10_96717f_write_reg(sensor, 0x0311, 0x40);
		ox03j10_96717f_write_reg(sensor, 0x0308, 0x64);
		ox03j10_96717f_write_reg(sensor, 0x0318, 0x5e);
		ox03j10_96717f_write_reg(sensor, 0x0002, 0x43);

		ox03j10_96717f_write_reg(sensor, 0x0044, (CAM_TRANS_ADDR_BASE+i)<<1); //dst addr
		ox03j10_96717f_write_reg(sensor, 0x0045, CAM_DEF_ADDR<<1); //src addr, original sensor addr

		ox03j10_96717f_write_reg(sensor, 0x0570, 0x00);
		ox03j10_96717f_write_reg(sensor, 0x03f0, 0x59);
		ox03j10_96717f_write_reg(sensor, 0x03f1, 0x89);

		ox03j10_96717f_write_reg(sensor,0x007b, 0x30+i);
		ox03j10_96717f_write_reg(sensor,0x0083, 0x30+i);
		ox03j10_96717f_write_reg(sensor,0x0093, 0x30+i);
		ox03j10_96717f_write_reg(sensor,0x009b, 0x30+i);
		ox03j10_96717f_write_reg(sensor,0x00a3, 0x30+i);
		ox03j10_96717f_write_reg(sensor,0x00ab, 0x30+i);
		ox03j10_96717f_write_reg(sensor,0x008b, 0x30+i);
	}

	client->addr = 0x29;
	restore_link(sensor);

	client->addr = addrbackup;
	return 0;
error:
	client->addr = addrbackup;
	return -EINVAL;
}

static ssize_t zuojisi_links_show(struct device *dev, struct device_attribute*attr, char *buf)  
{
    u32 idx;
	struct ox03j10_96717f_dev* sensor =  v4l2_get_subdevdata(dev_get_drvdata(dev));

	dev_info(dev, "zuojisi: get link status=0x%x\n", sensor->link_lock_status);

    for(idx=0;idx<4;idx++){
        if( sensor->link_lock_status & (1<<idx)){
            buf[2*idx] = '1';
            buf[2*idx+1] = ' ';
        }else{
            buf[2*idx] = '0';
            buf[2*idx+1] = ' ';
        }
    }
    buf[8] = 0;
    return 9;
}
 
static DEVICE_ATTR(zuojisi_links, S_IRUSR |S_IRUGO, zuojisi_links_show, NULL);  

static int ox03j10_96717f_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct ox03j10_96717f_dev *sensor;
	int ret;

	dev_info(dev, "zuojisi: probing ox03j10_96717f...\n");

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev),
						  NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		return ret;
	}

	if (sensor->ep.bus_type != V4L2_MBUS_PARALLEL &&
	    sensor->ep.bus_type != V4L2_MBUS_CSI2_DPHY &&
	    sensor->ep.bus_type != V4L2_MBUS_BT656) {
		dev_err(dev, "Unsupported bus type %d\n", sensor->ep.bus_type);
		return -EINVAL;
	}

	/* get system clock (xclk) */
	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(sensor->xclk);
	}

	/* request optional power down pin */
	sensor->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn_gpio))
		return PTR_ERR(sensor->pwdn_gpio);

	/* request optional reset pin */
	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return PTR_ERR(sensor->reset_gpio);

	v4l2_i2c_subdev_init(&sensor->sd, client, &ox03j10_96717f_subdev_ops);
	v4l2_set_subdevdata(&sensor->sd,sensor);
	sensor->sd.internal_ops = &ox03j10_96717f_internal_ops;

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	sensor->pad[0].flags = MEDIA_PAD_FL_SOURCE;
	sensor->pad[1].flags = MEDIA_PAD_FL_SOURCE;
	sensor->pad[2].flags = MEDIA_PAD_FL_SOURCE;
	sensor->pad[3].flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 4, sensor->pad);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto entity_cleanup;

	ret = ox03j10_96717f_get_regulators(sensor);
	if (ret)
		goto entity_cleanup;

	mutex_init(&sensor->lock);

	ret = ox03j10_96717f_setup_deserializer(sensor);
	if (ret) {
		dev_err(dev, "failed to setup deserializer\n");
		goto entity_cleanup;
	}

	ret = ox03j10_96717f_sensor_resume(dev);
	if (ret) {
		dev_err(dev, "failed to power on\n");
		goto entity_cleanup;
	}

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_pm_runtime;

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	device_create_file(dev,&dev_attr_zuojisi_links);

	dev_info(dev, "zuojisi: probe ox03j10_96717f success!\n");

	return 0;

err_pm_runtime:
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	ox03j10_96717f_sensor_suspend(dev);
entity_cleanup:
	v4l2_subdev_cleanup(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	mutex_destroy(&sensor->lock);
	return ret;
}

static void ox03j10_96717f_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ox03j10_96717f_dev *sensor = to_ox03j10_96717f_dev(sd);
	struct device *dev = &client->dev;

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		ox03j10_96717f_sensor_suspend(dev);
	pm_runtime_set_suspended(dev);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_subdev_cleanup(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	mutex_destroy(&sensor->lock);
}

static const struct dev_pm_ops ox03j10_96717f_pm_ops = {
	SET_RUNTIME_PM_OPS(ox03j10_96717f_sensor_suspend, ox03j10_96717f_sensor_resume, NULL)
};

static const struct i2c_device_id ox03j10_96717f_id[] = {
	{"ox03j10_96717f", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ox03j10_96717f_id);

static const struct of_device_id ox03j10_96717f_dt_ids[] = {
	{ .compatible = "zuojisi,z_xcdes30_xcconv11_ox03j10_96717f" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ox03j10_96717f_dt_ids);

static struct i2c_driver ox03j10_96717f_i2c_driver = {
	.driver = {
		.name  = "z_xcdes30_xcconv11_ox03j10_96717f",
		.of_match_table	= ox03j10_96717f_dt_ids,
		.pm = &ox03j10_96717f_pm_ops,
	},
	.id_table = ox03j10_96717f_id,
	.probe    = ox03j10_96717f_probe,
	.remove   = ox03j10_96717f_remove,
};

module_i2c_driver(ox03j10_96717f_i2c_driver);

MODULE_DESCRIPTION("OX03J10_96717F MIPI Camera Subdev Driver by zuojisi@163.com");
MODULE_LICENSE("GPL");
