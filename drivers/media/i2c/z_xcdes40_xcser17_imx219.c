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

#define Z_NUM_PADS 4

/* Analog gain control */
#define IMX219_REG_ANALOG_GAIN		(0x0157)
#define IMX219_ANA_GAIN_MIN		0
#define IMX219_ANA_GAIN_MAX		232
#define IMX219_ANA_GAIN_STEP		1
#define IMX219_ANA_GAIN_DEFAULT		0x0

/* Digital gain control */
#define IMX219_REG_DIGITAL_GAIN		(0x0158)
#define IMX219_DGTL_GAIN_MIN		0x0100
#define IMX219_DGTL_GAIN_MAX		0x0fff
#define IMX219_DGTL_GAIN_DEFAULT	0x0100
#define IMX219_DGTL_GAIN_STEP		1

/* Exposure control */
#define IMX219_REG_EXPOSURE		(0x015a)
#define IMX219_EXPOSURE_MIN		4
#define IMX219_EXPOSURE_STEP		1
#define IMX219_EXPOSURE_DEFAULT		0x640
#define IMX219_EXPOSURE_MAX		65535

/* V_TIMING internal */
#define IMX219_REG_VTS			(0x0160)
#define IMX219_VTS_15FPS		0x0dc6
#define IMX219_VTS_30FPS_1080P		0x06e3
#define IMX219_VTS_30FPS_BINNED		0x06e3
#define IMX219_VTS_30FPS_640x480	0x06e3
#define IMX219_VTS_MAX			0xffff

#define IMX219_VBLANK_MIN		32

/*Frame Length Line*/
#define IMX219_FLL_MIN			0x08a6
#define IMX219_FLL_MAX			0xffff
#define IMX219_FLL_STEP			1
#define IMX219_FLL_DEFAULT		0x0c98

/* HBLANK control range */
#define IMX219_PPL_MIN			3448
#define IMX219_PPL_MAX			0x7ff0
#define IMX219_REG_HTS			(0x0162)

/* Pixel rate is fixed for all the modes */
#define IMX219_PIXEL_RATE		182400000
#define IMX219_PIXEL_RATE_4LANE		281600000

#define max96724_PHY_CLK (0x20|16)
#define max96724_TX11_PIPE_X_EN_ADDR 0x90B
#define max96724_TX45_PIPE_X_DST_CTRL_ADDR 0x92D
#define max96724_PIPE_X_SRC_0_MAP_ADDR 0x90D

#define 	FMT_CODE (MEDIA_BUS_FMT_SRGGB10_1X10)

/* regulator supplies */
static const char * const imx219_supply_name[] = {
	"DOVDD", /* Digital I/O (1.8V) supply */
	"AVDD",  /* Analog (2.8V) supply */
	"DVDD",  /* Digital Core (1.5V) supply */
};

#define imx219_NUM_SUPPLIES ARRAY_SIZE(imx219_supply_name)

struct reg_value {
	u16 reg_addr;
	u8 val;
	u8 mask;
	u32 delay_ms;
};

struct imx219_dev {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad[4];
	struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */
	struct clk *xclk; /* system clock to imx219 */
	u32 xclk_freq;

	struct regulator_bulk_data supplies[imx219_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	bool   upside_down;

	/* lock to protect all members below */
	struct mutex lock;
	bool streaming;
	u8 link_lock_status;

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *pixel_rate;
	// struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	// struct v4l2_ctrl *vflip;
	// struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
};

static inline struct imx219_dev *to_imx219_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx219_dev, sd);
}

static struct v4l2_mbus_framefmt imx219_default_fmt = {
	.code = FMT_CODE,
	.width = 1920,
	.height = 1080,
	.colorspace = V4L2_COLORSPACE_REC709,
	.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_REC709),
	.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(V4L2_COLORSPACE_REC709),
	.field = V4L2_FIELD_NONE,
};

static const struct reg_value imx219_init_setting[] = {
    /* software reset */
    {0x0103, 0x01, 0, 30},
    /* sensor config */
    {0x0114, 0x01, 0, 0}, /* D-Phy, 2-lane */
    {0x0128, 0x00, 0, 0},
    {0x012A, 0x18, 0, 0}, /* 24 MHz INCK */
    {0x012B, 0x00, 0, 0},
    /* access code - vendor addr. ranges */
    {0x30EB, 0x05, 0, 0},
    {0x30EB, 0x0C, 0, 0},
    {0x300A, 0xFF, 0, 0},
    {0x300B, 0xFF, 0, 0},
    {0x30EB, 0x05, 0, 0},
    {0x30EB, 0x09, 0, 0},
    /* cis tuning */
    {0x455E, 0x00, 0, 0},
    {0x471E, 0x4B, 0, 0},
    {0x4767, 0x0F, 0, 0},
    {0x4750, 0x14, 0, 0},
    {0x4540, 0x00, 0, 0},
    {0x47B4, 0x14, 0, 0},
    {0x4713, 0x30, 0, 0},
    {0x478B, 0x10, 0, 0},
    {0x478F, 0x10, 0, 0},
    {0x4793, 0x10, 0, 0},
    {0x4797, 0x0E, 0, 0},
    {0x479B, 0x0E, 0, 100},
 
    /* capture settings */
    {0x0157, 0x10, 0, 0}, /* ANALOG_GAIN_GLOBAL[7:0] */
    {0x015A, 0x06, 0, 0}, /* COARSE_INTEG_TIME[15:8] */
    {0x015B, 0xde, 0, 0}, /* COARSE_INTEG_TIME[7:0] */
    /* format settings */
    {0x0160, 0x06, 0, 0}, /* FRM_LENGTH[15:8] */
    {0x0161, 0xE2, 0, 0}, /* FRM_LENGTH[7:0] */
    {0x0162, 0x0D, 0, 0}, /* LINE_LENGTH[15:8] */
    {0x0163, 0x78, 0, 0}, /* LINE_LENGTH[7:0] */
    {0x0164, 0x02, 0, 0},
    {0x0165, 0xA8, 0, 0},
    {0x0166, 0x0A, 0, 0},
    {0x0167, 0x27, 0, 0},
    {0x0168, 0x02, 0, 0},
    {0x0169, 0xB4, 0, 0},
    {0x016A, 0x06, 0, 0},
    {0x016B, 0xEB, 0, 0},
    {0x016C, 0x07, 0, 0},
    {0x016D, 0x80, 0, 0},
    {0x016E, 0x04, 0, 0},
    {0x016F, 0x38, 0, 0},
    {0x0170, 0x01, 0, 0},
    {0x0171, 0x01, 0, 0},
    {0x0174, 0x00, 0, 0},
    {0x0175, 0x00, 0, 0},
    {0x018C, 0x0A, 0, 0},
    {0x018D, 0x0A, 0, 0},
    /* clocks dividers */
    {0x0301, 0x05, 0, 0},
    {0x0303, 0x01, 0, 0},
    {0x0304, 0x03, 0, 0},
    {0x0305, 0x03, 0, 0},
    {0x0306, 0x00, 0, 0},
    {0x0307, 0x39, 0, 0},
    {0x0309, 0x0A, 0, 0},
    {0x030B, 0x01, 0, 0},
    {0x030C, 0x00, 0, 0},
    {0x030D, 0x72, 0, 30}
};

static const struct reg_value imx219_start_stream[] = {
    {0x0100, 0x01, 0, 10},
};

static const struct reg_value imx219_stop_stream[] = {
	{0x0100, 0x00, 0, 10} //SW Power Down, wait 10ms
};

static int imx219_write_reg(struct imx219_dev *sensor, u16 reg, u8 val)
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

static int imx219_read_reg(struct imx219_dev *sensor, u16 reg, u8 *val)
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
	dev_info(&client->dev, "zuojisi: w 0x%02x : (0x%04x)<-0x%02x\n",client->addr,reg, *val);
	return 0;
}

static int imx219_mod_reg(struct imx219_dev *sensor, u16 reg,
			  u8 mask, u8 val)
{
	u8 readval;
	int ret;

	ret = imx219_read_reg(sensor, reg, &readval);
	if (ret)
		return ret;

	readval &= ~mask;
	val &= mask;
	val |= readval;

	return imx219_write_reg(sensor, reg, val);
}


static void imx219_load_regs(struct imx219_dev *sensor,
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
			ret = imx219_mod_reg(sensor, reg_addr, mask, val);
		else
			ret = imx219_write_reg(sensor, reg_addr, val);
		if (ret)
			break;

		if (delay_ms)
			usleep_range(1000 * delay_ms, 1000 * delay_ms + 100);
	}
}


/* restore the last set video mode after chip power-on */
static int imx219_init_base_settings(struct imx219_dev *sensor)
{
	/* first load the initial register values */
	imx219_load_regs(sensor, imx219_init_setting, ARRAY_SIZE(imx219_init_setting));

	return 0;
}

static void imx219_power(struct imx219_dev *sensor, bool enable)
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
static void imx219_powerup_sequence(struct imx219_dev *sensor)
{
	if (sensor->pwdn_gpio) {
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);

		/* camera power cycle */
		imx219_power(sensor, false);
		usleep_range(5000, 10000);
		imx219_power(sensor, true);
		usleep_range(5000, 10000);

		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
		usleep_range(1000, 2000);

		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	}
	usleep_range(20000, 25000);

}

static int imx219_set_power_on(struct imx219_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	int ret;

	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		return ret;
	}

	ret = regulator_bulk_enable(imx219_NUM_SUPPLIES,
				    sensor->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		goto xclk_off;
	}

	imx219_powerup_sequence(sensor);

	return 0;

xclk_off:
	clk_disable_unprepare(sensor->xclk);
	return ret;
}

static void imx219_set_power_off(struct imx219_dev *sensor)
{
	imx219_power(sensor, false);
	regulator_bulk_disable(imx219_NUM_SUPPLIES, sensor->supplies);
	clk_disable_unprepare(sensor->xclk);
}


static int imx219_set_power(struct imx219_dev *sensor, bool on)
{
	int ret = 0;

	if (on) {
		ret = imx219_set_power_on(sensor);
		if (ret)
			return ret;

		ret = imx219_init_base_settings(sensor);
		if (ret)
			goto power_off;
	}

	if (!on)
		imx219_set_power_off(sensor);

	return 0;

power_off:
	imx219_set_power_off(sensor);
	return ret;
}

static int imx219_sensor_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx219_dev *imx219 = to_imx219_dev(sd);

	return imx219_set_power(imx219, false);
}

static int imx219_sensor_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx219_dev *imx219 = to_imx219_dev(sd);

	return imx219_set_power(imx219, true);
}

/* --------------- Subdev Operations --------------- */
static int imx219_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct imx219_dev *sensor = to_imx219_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s , pad=%d\n",__func__, format->pad);

	if (format->pad > 3)
		return -EINVAL;

	mutex_lock(&sensor->lock);
	format->format = imx219_default_fmt;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int imx219_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct imx219_dev *sensor = to_imx219_dev(sd);
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

	imx219_default_fmt = format->format;

	ret = 0;
out:
	mutex_unlock(&sensor->lock);
	return ret;
}

static int imx219_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct imx219_dev *sensor = to_imx219_dev(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		mutex_lock(&sensor->lock);
		sel->r.left = sel->r.top = 0;
		sel->r.width = 1920;
		sel->r.height = 1080;
		mutex_unlock(&sensor->lock);
		return 0;
	}

	return -EINVAL;
}

static int imx219_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned int index = fse->index;
	struct imx219_dev *sensor = to_imx219_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	if (fse->pad > 3)
		return -EINVAL;

	if (index != 0)
		return -EINVAL;

	fse->min_width = 1920;
	fse->max_width = fse->min_width;
	fse->min_height = 1080;
	fse->max_height = fse->min_height;

	return 0;
}

static int imx219_enum_frame_interval(
	struct v4l2_subdev *sd,
	struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval_enum *fie)
{
	struct v4l2_fract tpf;
	struct imx219_dev *sensor = to_imx219_dev(sd);
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

static int imx219_get_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx219_dev *sensor = to_imx219_dev(sd);
	dev_info(&sensor->i2c_client->dev, "zuojisi: %s \n",__func__);

	mutex_lock(&sensor->lock);
	fi->interval.numerator = 1;
	fi->interval.denominator = 30;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int imx219_set_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx219_dev *sensor = to_imx219_dev(sd);
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

static int imx219_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;

	code->code = FMT_CODE;

	return 0;
}

static int imx219_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx219_dev *sensor = to_imx219_dev(sd);
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
		imx219_load_regs(sensor, imx219_start_stream, ARRAY_SIZE(imx219_start_stream));
		addrbackup = sensor->i2c_client->addr;

		sensor->i2c_client->addr = 0x27;
		imx219_write_reg(sensor,0x00f4, sensor->link_lock_status);
		imx219_write_reg(sensor,0x0018, sensor->link_lock_status);

		imx219_write_reg(sensor,0x0943,0x10);
		imx219_write_reg(sensor,0x0943,0x33);
		imx219_write_reg(sensor,0x0983,0x10);
		imx219_write_reg(sensor,0x0983,0x33);

		mdelay(100);
		dev_info(&sensor->i2c_client->dev, "zuojisi: link lock status =%x\n",sensor->link_lock_status);

		sensor->i2c_client->addr = addrbackup;

	} else {
		imx219_load_regs(sensor, imx219_stop_stream, ARRAY_SIZE(imx219_stop_stream));
	}

	sensor->streaming = enable;
	mutex_unlock(&sensor->lock);

	if (!enable || ret)
		pm_runtime_put_autosuspend(&sensor->i2c_client->dev);

	return ret;
}

static int imx219_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt =
				v4l2_subdev_state_get_format(state, 0);
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);

	*fmt = imx219_default_fmt;

	crop->left = 0;
	crop->top = 0;
	crop->width = 1920;
	crop->height = 1080;

	return 0;
}

static int imx219_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad, struct v4l2_mbus_frame_desc *fd)
{
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].bus.csi2.dt = 0x2b;
	fd->entry[0].bus.csi2.vc = (u8)pad;
	return 0;
}

static const struct v4l2_subdev_core_ops imx219_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx219_video_ops = {
	.s_stream = imx219_s_stream,
};

static const struct v4l2_subdev_pad_ops imx219_pad_ops = {
	.enum_mbus_code = imx219_enum_mbus_code,
	.get_fmt = imx219_get_fmt,
	.set_fmt = imx219_set_fmt,
	.get_selection = imx219_get_selection,
	.enum_frame_size = imx219_enum_frame_size,
	.enum_frame_interval = imx219_enum_frame_interval,
	.get_frame_desc = imx219_get_frame_desc,
	.get_frame_interval = imx219_get_frame_interval,
	.set_frame_interval = imx219_set_frame_interval,
};

static int imx219_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx219_dev *imx219 =
		container_of(ctrl->handler, struct imx219_dev, ctrl_handler);
	struct i2c_client *client = imx219->i2c_client;
	int ret = 0;
	// const struct v4l2_mbus_framefmt *format;
	// struct v4l2_subdev_state *state;
	// int rate_factor;

	dev_info(&client->dev, "zuojisi: ctrl(id:0x%x,val:0x%x) received\n", ctrl->id, ctrl->val);

	// if (ctrl->id == V4L2_CID_VBLANK) {
	// 	int exposure_max, exposure_def;

	// 	/* Update max exposure while meeting expected vblanking */
	// 	exposure_max = imx219->mode->height + ctrl->val - 4;
	// 	exposure_def = (exposure_max < IMX219_EXPOSURE_DEFAULT) ?
	// 		exposure_max : IMX219_EXPOSURE_DEFAULT;
	// 	__v4l2_ctrl_modify_range(imx219->exposure,
	// 				 imx219->exposure->minimum,
	// 				 exposure_max, imx219->exposure->step,
	// 				 exposure_def);
	// }

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	// state = v4l2_subdev_get_locked_active_state(&imx219->sd);
	// format = v4l2_subdev_get_pad_format(&imx219->sd, state, 0);
	// rate_factor = imx219_get_rate_factor(imx219, format);

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx219_write_reg(imx219,IMX219_REG_ANALOG_GAIN,ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret  = imx219_write_reg(imx219,IMX219_REG_EXPOSURE,(ctrl->val & 0xff00)>>8);
		ret |= imx219_write_reg(imx219,IMX219_REG_EXPOSURE+1,ctrl->val & 0xff);
		break;
	case V4L2_CID_VBLANK:
		ret  = imx219_write_reg(imx219,IMX219_REG_VTS,(ctrl->val & 0xff00)>>8);
		ret |= imx219_write_reg(imx219,IMX219_REG_VTS+1,ctrl->val & 0xff);
		break;
	case V4L2_CID_HBLANK:
		ret  = imx219_write_reg(imx219,IMX219_REG_HTS,(ctrl->val & 0xff00)>>8);
		ret |= imx219_write_reg(imx219,IMX219_REG_HTS+1,ctrl->val & 0xff);
		break;

	default:
		dev_info(&client->dev, "zuojisi: ctrl(id:0x%x,val:0x%x) is not handled\n", ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}


static const struct v4l2_ctrl_ops imx219_ctrl_ops = {
	.s_ctrl = imx219_set_ctrl,
};

/* Initialize control handlers */
static int imx219_init_controls(struct imx219_dev *imx219)
{
	struct i2c_client *client = imx219->i2c_client;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	unsigned int height = 1080;
	unsigned int width = 1920;
	struct v4l2_fwnode_device_properties props;
	int exposure_max, exposure_def, hblank, pixel_rate;
	int i, ret;

	ctrl_hdlr = &imx219->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 4);
	if (ret)
		return ret;

	/* By default, PIXEL_RATE is read only */
	pixel_rate = (max96724_PHY_CLK&0x1f)*100000000/10*4;

	imx219->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx219_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       pixel_rate, pixel_rate, 1,
					       pixel_rate);
	/* Initial vblank/hblank/exposure parameters based on current mode */
	imx219->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx219_ctrl_ops,
					   V4L2_CID_VBLANK, IMX219_VBLANK_MIN,
					   IMX219_VTS_MAX - height, 1,
					   IMX219_VTS_30FPS_1080P - height);
	hblank = IMX219_PPL_MIN - width;
	imx219->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx219_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   IMX219_PPL_MIN - width,
					   1, hblank);
	exposure_max = IMX219_VTS_30FPS_1080P - 4;
	exposure_def = (exposure_max < IMX219_EXPOSURE_DEFAULT) ? exposure_max : IMX219_EXPOSURE_DEFAULT;
	imx219->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx219_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX219_EXPOSURE_MIN, exposure_max,
					     IMX219_EXPOSURE_STEP,
					     exposure_def);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx219_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX219_ANA_GAIN_MIN, IMX219_ANA_GAIN_MAX,
			  IMX219_ANA_GAIN_STEP, IMX219_ANA_GAIN_DEFAULT);


	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "zuojisi %s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx219_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	imx219->sd.ctrl_handler = ctrl_hdlr;

	dev_err(&client->dev, "zuojisi: control init finished OK\n");
	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}


static const struct v4l2_subdev_internal_ops imx219_internal_ops = {
	.init_state = imx219_init_cfg,
};

static const struct v4l2_subdev_ops imx219_subdev_ops = {
	.core = &imx219_core_ops,
	.video = &imx219_video_ops,
	.pad = &imx219_pad_ops,
};

static int imx219_get_regulators(struct imx219_dev *sensor)
{
	int i;

	for (i = 0; i < imx219_NUM_SUPPLIES; i++)
		sensor->supplies[i].supply = imx219_supply_name[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       imx219_NUM_SUPPLIES,
				       sensor->supplies);
}


static int check_version(struct imx219_dev *sensor)
{
	const uint16_t ver0addr = 0x0316; //ver0 : mfp7
	const uint16_t ver1addr = 0x0319; //ver1 : mfp8

	uint16_t veraddrs[2] = {ver0addr,ver1addr};
	u8 ver=0,tmp,i;

	for(i=0;i<2;i++){
		imx219_read_reg(sensor,veraddrs[i],&tmp);
		tmp = (tmp & 0x8)>>3;
		ver = ver | (tmp<<i);
	}

	return (ver==0x01)?0:-1;
}

static int get_link_lock_status(struct imx219_dev *sensor)
{
	u8 val;
	sensor->link_lock_status = 0;
	imx219_read_reg(sensor,0x001a,&val);
	sensor->link_lock_status |= (val & 0x08)>>3;
	imx219_read_reg(sensor,0x000a,&val);
	sensor->link_lock_status |= (val & 0x08)>>2;
	imx219_read_reg(sensor,0x000b,&val);
	sensor->link_lock_status |= (val & 0x08)>>1;
	imx219_read_reg(sensor,0x000c,&val);
	sensor->link_lock_status |= (val & 0x08)>>0;
	return 0;
}

static int monopolize_link(struct imx219_dev *sensor, int link)
{
	u8 value;
	struct i2c_client *client = sensor->i2c_client;
	struct device *dev = &client->dev;

	dev_info(dev, "zuojisi: %s: monopolize link_%c\n", __func__, 'A'+link);

	imx219_read_reg(sensor,0x0006,&value);
	value = (value & 0xF0) | (1<<link);
	imx219_write_reg(sensor,0x0006,value);
	mdelay(150);
	return 0;
}

static int restore_link(struct imx219_dev *sensor)
{
	imx219_write_reg(sensor,0x0006,0xF0 | sensor->link_lock_status);
	mdelay(150);
	return 0;
}

static int imx219_setup_deserializer(struct imx219_dev *sensor)
{
	struct i2c_client *client = sensor->i2c_client;
	struct device *dev = &client->dev;
	u16 addrbackup = client->addr;
	u8 val = 0;
	int ret,i;
	client->addr = 0x27;

	ret = imx219_read_reg(sensor, 0x0000, &val);
	if(ret == 0 && val == 0x4e){
		dev_info(dev, "zuojisi: deserializer detected\n");
	}else{
		dev_err(dev, "zuojisi: deserializer NOT detected\n");
		goto error;
	}

	if(check_version(sensor)){
		return -EINVAL;
	}

	imx219_write_reg(sensor,0x0013,0x40); //reset all
	mdelay(100);
	imx219_write_reg(sensor,0x0013,0x00);

	imx219_write_reg(sensor,0x0005,0x88);
	imx219_write_reg(sensor,0x0310,0x90); 

	//turn on LED
	imx219_write_reg(sensor,0x0309,0x90);

	get_link_lock_status(sensor);
	//disable Links which are not Locked
	imx219_read_reg(sensor,0x0006,&val);
	imx219_write_reg(sensor,0x0006,(val&0xF0)|(sensor->link_lock_status&0x0F));

	dev_err(dev, "zuojisi: link lock status=0x%02x\n",sensor->link_lock_status);
	if(sensor->link_lock_status == 0){
		dev_err(dev, "zuojisi: no camera detected\n");
		return -EINVAL;
	}

	imx219_write_reg(sensor,0x90a,0xc0); //4 lanes, D-Phy, 2bits VC
	imx219_write_reg(sensor,0x94a,0xc0); //4 lanes, D-Phy, 2bits VC
	imx219_write_reg(sensor,0x98a,0xc0); //4 lanes, D-Phy, 2bits VC
	imx219_write_reg(sensor,0x9ca,0xc0); //4 lanes, D-Phy, 2bits VC

	imx219_write_reg(sensor,0x8a3,0xe4); //phy lane mapping
	imx219_write_reg(sensor,0x8a4,0xe4); //phy lane mapping

    imx219_write_reg(sensor,0x8a0,0x24); // 2x4 mode

	imx219_write_reg(sensor,0x415,max96724_PHY_CLK);
	imx219_write_reg(sensor,0x418,max96724_PHY_CLK);
	imx219_write_reg(sensor,0x41b,max96724_PHY_CLK);
	imx219_write_reg(sensor,0x41e,max96724_PHY_CLK);

	imx219_write_reg(sensor,0xf0,0x62); //pipe1 GMSLB, Z-pipe; pipe0 GMSLA, Z-pipe
	imx219_write_reg(sensor,0xf1,0xea); //pipe3 GMSLD, Z-pipe; pipe2 GMSLC, Z-pipe
	imx219_write_reg(sensor,0xf4,0x00); //disable pipes 0-4 (default val)

	//mfp0, enable TX with ID=A
    imx219_write_reg(sensor,0x0300,0x83); //TX on A
	imx219_write_reg(sensor,0x0301,0xaa); //TX on A
	imx219_write_reg(sensor,0x0337,0x2a); //TX on B
	imx219_write_reg(sensor,0x036d,0x2a); //TX on C
	imx219_write_reg(sensor,0x03a4,0x2a); //TX on D

	imx219_write_reg(sensor,0x0027,0x00); //disable unconcerned error report
	imx219_write_reg(sensor,0x0029,0xf0); //disable unconcerned error report

	for(i=0;i<4;i++){
		if(! (sensor->link_lock_status & (0x01<<i) )){
			continue;
		}
		client->addr = 0x27;

		imx219_write_reg(sensor, (0x40*i)+max96724_TX11_PIPE_X_EN_ADDR+0, 0x1f);
		imx219_write_reg(sensor, (0x40*i)+max96724_TX45_PIPE_X_DST_CTRL_ADDR+0, 0xAA);  // 0x55=controller1  0xAA=controller2
		imx219_write_reg(sensor, (0x40*i)+max96724_TX45_PIPE_X_DST_CTRL_ADDR+1, 0x02); 
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+0, 0x2b);
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+1, 0x2b | (i << 6));
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+2, 0x00);
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+3, 0x00 | (i << 6));
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+4, 0x01);
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+5, 0x01 | (i << 6));
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+6, 0x02);
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+7, 0x02 | (i << 6));
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+8, 0x03);
		imx219_write_reg(sensor, (0x40*i)+max96724_PIPE_X_SRC_0_MAP_ADDR+9, 0x03 | (i << 6));

		monopolize_link(sensor,i);

		client->addr = 0x40;
		imx219_write_reg(sensor, 0x0000, (0x40+i+1)<<1);
		client->addr = 0x40+i+1;
		mdelay(75);

		ret = imx219_read_reg(sensor, 0x000d, &val);
		if(ret == 0 && (val == 0x91 || val == 0xbf)){
			dev_info(dev, "zuojisi: serializer detected\n");
		}else{
			dev_err(dev, "zuojisi: serializer NOT detected\n");
			sensor->link_lock_status &= ~(1<<i);
			goto error;
		}

		imx219_write_reg(sensor, 0x02bf, 0x60);
		imx219_write_reg(sensor, 0x02be, 0x10);

		imx219_write_reg(sensor, 0x0311, 0x40);
		imx219_write_reg(sensor, 0x0308, 0x64);
		imx219_write_reg(sensor, 0x0318, 0x6b);
		imx219_write_reg(sensor, 0x0002, 0x43);

		imx219_write_reg(sensor, 0x0330, 0x40);
		imx219_write_reg(sensor, 0x0331, 0x11); //2 lanes
		imx219_write_reg(sensor, 0x0334, 0x70); //phy1 for xcSer inverse polarity
		imx219_write_reg(sensor, 0x0335, 0x07); //phy2 for xcSer inverse polarity

		imx219_write_reg(sensor,0x007b, 0x30+i);
		imx219_write_reg(sensor,0x0083, 0x30+i);
		imx219_write_reg(sensor,0x0093, 0x30+i);
		imx219_write_reg(sensor,0x009b, 0x30+i);
		imx219_write_reg(sensor,0x00a3, 0x30+i);
		imx219_write_reg(sensor,0x00ab, 0x30+i);
		imx219_write_reg(sensor,0x008b, 0x30+i);
	}

	client->addr = 0x27;
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
	struct imx219_dev* sensor =  v4l2_get_subdevdata(dev_get_drvdata(dev));

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

static int imx219_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct imx219_dev *sensor;
	int ret;

	dev_info(dev, "zuojisi: probing imx219...\n");

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

	v4l2_i2c_subdev_init(&sensor->sd, client, &imx219_subdev_ops);
	v4l2_set_subdevdata(&sensor->sd,sensor);
	sensor->sd.internal_ops = &imx219_internal_ops;

	imx219_init_controls(sensor);

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	sensor->pad[0].flags = MEDIA_PAD_FL_SOURCE;
	sensor->pad[1].flags = MEDIA_PAD_FL_SOURCE;
	sensor->pad[2].flags = MEDIA_PAD_FL_SOURCE;
	sensor->pad[3].flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, Z_NUM_PADS, sensor->pad);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto entity_cleanup;

	ret = imx219_get_regulators(sensor);
	if (ret)
		goto entity_cleanup;

	mutex_init(&sensor->lock);

	ret = imx219_setup_deserializer(sensor);
	if (ret) {
		dev_err(dev, "failed to setup deserializer\n");
		goto entity_cleanup;
	}

	ret = imx219_sensor_resume(dev);
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

	dev_info(dev, "zuojisi: probe imx219 success!\n");

	return 0;

err_pm_runtime:
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	imx219_sensor_suspend(dev);
entity_cleanup:
	v4l2_subdev_cleanup(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	mutex_destroy(&sensor->lock);
	return ret;
}

static void imx219_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx219_dev *sensor = to_imx219_dev(sd);
	struct device *dev = &client->dev;

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		imx219_sensor_suspend(dev);
	pm_runtime_set_suspended(dev);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_subdev_cleanup(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	mutex_destroy(&sensor->lock);
}

static const struct dev_pm_ops imx219_pm_ops = {
	SET_RUNTIME_PM_OPS(imx219_sensor_suspend, imx219_sensor_resume, NULL)
};

static const struct i2c_device_id imx219_id[] = {
	{"imx219", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, imx219_id);

static const struct of_device_id imx219_dt_ids[] = {
	{ .compatible = "zuojisi,z_xcdes40_xcser17_imx219" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx219_dt_ids);

static struct i2c_driver imx219_i2c_driver = {
	.driver = {
		.name  = "z_xcdes40_xcser17_imx219",
		.of_match_table	= imx219_dt_ids,
		.pm = &imx219_pm_ops,
	},
	.id_table = imx219_id,
	.probe    = imx219_probe,
	.remove   = imx219_remove,
};

module_i2c_driver(imx219_i2c_driver);

MODULE_DESCRIPTION("imx219 MIPI Camera Subdev Driver by zuojisi@163.com");
MODULE_LICENSE("GPL");
