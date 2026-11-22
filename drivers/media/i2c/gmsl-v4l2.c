// SPDX-License-Identifier: GPL-2.0

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
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
#include <media/media-entity.h>
#include <media/v4l2-subdev.h>

#include "gmsl-core.h"
#include "gmsl-desc.h"

#define GMSL_NUM_SUPPLIES 3

static const char * const gmsl_supply_name[] = {
	"DOVDD", /* Digital I/O (1.8V) supply */
	"AVDD",  /* Analog (2.8V) supply */
	"DVDD",  /* Digital Core (1.5V) supply */
};

struct gmsl_camera {
	struct gmsl_dev gmsl;
	struct v4l2_subdev sd;
	struct media_pad pad[GMSL_MAX_LINKS];
	struct clk *xclk;
	struct regulator_bulk_data supplies[GMSL_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;
	struct mutex lock; /* protects streaming and link formats */
	bool streaming;
};

static inline struct gmsl_camera *to_gmsl_camera(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gmsl_camera, sd);
}

static void gmsl_camera_power(struct gmsl_camera *camera, bool enable)
{
	gpiod_set_value_cansleep(camera->pwdn_gpio, enable ? 0 : 1);
}

static void gmsl_camera_powerup_sequence(struct gmsl_camera *camera)
{
	if (camera->pwdn_gpio) {
		gpiod_set_value_cansleep(camera->reset_gpio, 0);

		/* camera power cycle */
		gmsl_camera_power(camera, false);
		usleep_range(5000, 10000);
		gmsl_camera_power(camera, true);
		usleep_range(5000, 10000);

		gpiod_set_value_cansleep(camera->reset_gpio, 1);
		usleep_range(1000, 2000);

		gpiod_set_value_cansleep(camera->reset_gpio, 0);
	}
	usleep_range(20000, 25000);
}

static int gmsl_camera_set_power_on(struct gmsl_camera *camera)
{
	struct device *dev = &camera->gmsl.client->dev;
	int ret;

	ret = clk_prepare_enable(camera->xclk);
	if (ret) {
		dev_err(dev, "%s: failed to enable clock\n", __func__);
		return ret;
	}

	ret = regulator_bulk_enable(GMSL_NUM_SUPPLIES, camera->supplies);
	if (ret) {
		dev_err(dev, "%s: failed to enable regulators\n", __func__);
		goto xclk_off;
	}

	gmsl_camera_powerup_sequence(camera);
	return 0;

xclk_off:
	clk_disable_unprepare(camera->xclk);
	return ret;
}

static void gmsl_camera_set_power_off(struct gmsl_camera *camera)
{
	gmsl_camera_power(camera, false);
	regulator_bulk_disable(GMSL_NUM_SUPPLIES, camera->supplies);
	clk_disable_unprepare(camera->xclk);
}

static int gmsl_camera_set_power(struct gmsl_camera *camera, bool on)
{
	int ret;

	if (on) {
		ret = gmsl_camera_set_power_on(camera);
		if (ret)
			return ret;

		ret = gmsl_core_sensor_init(&camera->gmsl);
		if (ret)
			goto power_off;
	} else {
		gmsl_camera_set_power_off(camera);
	}

	return 0;

power_off:
	gmsl_camera_set_power_off(camera);
	return ret;
}

static int gmsl_camera_sensor_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gmsl_camera *camera = to_gmsl_camera(sd);

	return gmsl_camera_set_power(camera, false);
}

static int gmsl_camera_sensor_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gmsl_camera *camera = to_gmsl_camera(sd);

	return gmsl_camera_set_power(camera, true);
}

static int gmsl_camera_get_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_format *format)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);

	if (format->pad >= GMSL_MAX_LINKS)
		return -EINVAL;

	mutex_lock(&camera->lock);
	format->format = camera->gmsl.links[format->pad].fmt;
	mutex_unlock(&camera->lock);

	return 0;
}

static int gmsl_camera_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_format *format)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	int ret = 0;

	if (format->pad >= GMSL_MAX_LINKS)
		return -EINVAL;

	mutex_lock(&camera->lock);

	if (camera->streaming) {
		ret = -EBUSY;
		goto out;
	}
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, format->pad) =
			format->format;
		goto out;
	}

	camera->gmsl.links[format->pad].fmt = format->format;
	ret = 0;
out:
	mutex_unlock(&camera->lock);
	return ret;
}

static int gmsl_camera_get_selection(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_selection *sel)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	struct gmsl_link *link;

	if (sel->pad >= GMSL_MAX_LINKS)
		return -EINVAL;

	link = &camera->gmsl.links[sel->pad];

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		mutex_lock(&camera->lock);
		sel->r.left = sel->r.top = 0;
		sel->r.width = link->fmt.width;
		sel->r.height = link->fmt.height;
		mutex_unlock(&camera->lock);
		return 0;
	}

	return -EINVAL;
}

static int gmsl_camera_enum_frame_size(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_frame_size_enum *fse)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	struct gmsl_link *link;

	if (fse->pad >= GMSL_MAX_LINKS || fse->index != 0)
		return -EINVAL;

	link = &camera->gmsl.links[fse->pad];
	if (!link->desc)
		return -EINVAL;

	fse->min_width = link->desc->width;
	fse->max_width = fse->min_width;
	fse->min_height = link->desc->height;
	fse->max_height = fse->min_height;

	return 0;
}

static int gmsl_camera_enum_frame_interval(
	struct v4l2_subdev *sd,
	struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval_enum *fie)
{
	struct v4l2_fract tpf;

	if (fie->pad >= GMSL_MAX_LINKS || fie->index != 0)
		return -EINVAL;

	tpf.numerator = 1;
	tpf.denominator = 30;
	fie->interval = tpf;

	return 0;
}

static int gmsl_camera_get_frame_interval(
	struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval *fi)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);

	mutex_lock(&camera->lock);
	fi->interval.numerator = 1;
	fi->interval.denominator = 30;
	mutex_unlock(&camera->lock);

	return 0;
}

static int gmsl_camera_set_frame_interval(
	struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval *fi)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	int ret = 0;

	if (fi->pad >= GMSL_MAX_LINKS)
		return -EINVAL;

	mutex_lock(&camera->lock);

	if (camera->streaming) {
		ret = -EBUSY;
		goto out;
	}

	mutex_unlock(&camera->lock);
	return 0; /* fixed 30 fps */
out:
	mutex_unlock(&camera->lock);
	return ret;
}

static int gmsl_camera_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);

	if (code->pad >= GMSL_MAX_LINKS || code->index != 0)
		return -EINVAL;

	code->code = camera->gmsl.links[code->pad].fmt.code;

	return 0;
}

static u8 gmsl_camera_active_links(struct gmsl_camera *camera)
{
	u8 mask = 0;
	int i;

	for (i = 0; i < GMSL_MAX_LINKS; i++) {
		if (media_pad_is_streaming(&camera->sd.entity.pads[i]))
			mask |= BIT(i);
	}

	return mask;
}

static int gmsl_camera_stream(struct gmsl_camera *camera, u8 mask, bool enable)
{
	struct device *dev = &camera->gmsl.client->dev;
	int ret = 0;

	if (!mask)
		return enable ? -EINVAL : 0;

	mutex_lock(&camera->lock);

	if (enable && !camera->gmsl.streaming_mask) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret)
			goto out;
	}

	ret = gmsl_core_s_stream(&camera->gmsl, mask, enable);
	if (ret) {
		if (enable && !camera->gmsl.streaming_mask)
			pm_runtime_put_autosuspend(dev);
		goto out;
	}

	camera->streaming = !!camera->gmsl.streaming_mask;
	if (!enable && !camera->gmsl.streaming_mask)
		pm_runtime_put_autosuspend(dev);

out:
	mutex_unlock(&camera->lock);
	return ret;
}

static int gmsl_camera_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	u8 mask;

	if (enable) {
		mask = gmsl_camera_active_links(camera);
		if (!mask)
			mask = camera->gmsl.link_lock_status;
		dev_info(&camera->gmsl.client->dev,
			 "max96724-imx390: s stream enable=1 links=0x%x\n",
			 mask);
		return gmsl_camera_stream(camera, mask, true);
	}

	mask = camera->gmsl.streaming_mask;
	dev_info(&camera->gmsl.client->dev,
		 "max96724-imx390: s stream enable=0 links=0x%x\n", mask);
	return gmsl_camera_stream(camera, mask, false);
}

static int gmsl_camera_enable_streams(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      u32 pad, u64 streams_mask)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);

	if (pad >= GMSL_MAX_LINKS || streams_mask != BIT(0))
		return -EINVAL;
	if (!camera->gmsl.links[pad].desc)
		return -ENODEV;

	dev_info(&camera->gmsl.client->dev,
		 "max96724-imx390: enable streams pad=%u\n", pad);

	return gmsl_camera_stream(camera, BIT(pad), true);
}

static int gmsl_camera_disable_streams(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       u32 pad, u64 streams_mask)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);

	if (pad >= GMSL_MAX_LINKS || streams_mask != BIT(0))
		return -EINVAL;

	dev_info(&camera->gmsl.client->dev,
		 "max96724-imx390: disable streams pad=%u\n", pad);

	return gmsl_camera_stream(camera, BIT(pad), false);
}

static int gmsl_camera_init_cfg(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_rect *crop;
	int i;

	for (i = 0; i < GMSL_MAX_LINKS; i++) {
		fmt = v4l2_subdev_state_get_format(state, i);
		crop = v4l2_subdev_state_get_crop(state, i);

		*fmt = camera->gmsl.links[i].fmt;

		crop->left = 0;
		crop->top = 0;
		crop->width = fmt->width;
		crop->height = fmt->height;
	}

	return 0;
}

static int gmsl_camera_get_frame_desc(struct v4l2_subdev *sd,
				      unsigned int pad,
				      struct v4l2_mbus_frame_desc *fd)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	struct gmsl_link *link;

	if (pad >= GMSL_MAX_LINKS)
		return -EINVAL;

	link = &camera->gmsl.links[pad];
	if (!link->desc)
		return -EINVAL;

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].bus.csi2.dt = link->desc->csi_dt;
	fd->entry[0].bus.csi2.vc = (u8)pad;

	return 0;
}

static int gmsl_camera_get_mbus_config(struct v4l2_subdev *sd,
				       unsigned int pad,
				       struct v4l2_mbus_config *cfg)
{
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	struct gmsl_link *link;

	if (pad >= GMSL_MAX_LINKS)
		return -EINVAL;

	link = &camera->gmsl.links[pad];
	if (link->desc && link->desc->sensor_ops->get_mbus_config)
		return link->desc->sensor_ops->get_mbus_config(link, cfg);

	*cfg = camera->gmsl.mbus_config;
	return 0;
}

static const struct v4l2_subdev_core_ops gmsl_camera_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops gmsl_camera_video_ops = {
	.s_stream = gmsl_camera_s_stream,
};

static const struct v4l2_subdev_pad_ops gmsl_camera_pad_ops = {
	.enum_mbus_code = gmsl_camera_enum_mbus_code,
	.get_fmt = gmsl_camera_get_fmt,
	.set_fmt = gmsl_camera_set_fmt,
	.get_selection = gmsl_camera_get_selection,
	.enum_frame_size = gmsl_camera_enum_frame_size,
	.enum_frame_interval = gmsl_camera_enum_frame_interval,
	.get_frame_desc = gmsl_camera_get_frame_desc,
	.get_frame_interval = gmsl_camera_get_frame_interval,
	.set_frame_interval = gmsl_camera_set_frame_interval,
	.get_mbus_config = gmsl_camera_get_mbus_config,
	.enable_streams = gmsl_camera_enable_streams,
	.disable_streams = gmsl_camera_disable_streams,
};

static const struct v4l2_subdev_internal_ops gmsl_camera_internal_ops = {
	.init_state = gmsl_camera_init_cfg,
};

static const struct v4l2_subdev_ops gmsl_camera_subdev_ops = {
	.core = &gmsl_camera_core_ops,
	.video = &gmsl_camera_video_ops,
	.pad = &gmsl_camera_pad_ops,
};

static int gmsl_camera_get_regulators(struct gmsl_camera *camera)
{
	struct device *dev = &camera->gmsl.client->dev;
	int i;

	for (i = 0; i < GMSL_NUM_SUPPLIES; i++)
		camera->supplies[i].supply = gmsl_supply_name[i];

	return devm_regulator_bulk_get(dev, GMSL_NUM_SUPPLIES,
				       camera->supplies);
}

static ssize_t gmsl_links_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct gmsl_camera *camera =
		v4l2_get_subdevdata(dev_get_drvdata(dev));
	u32 idx;

	dev_dbg(dev, "max96724-imx390: get link status=0x%x\n",
		camera->gmsl.link_lock_status);

	for (idx = 0; idx < GMSL_MAX_LINKS; idx++) {
		if (camera->gmsl.link_lock_status & BIT(idx)) {
			buf[2 * idx] = '1';
			buf[2 * idx + 1] = ' ';
		} else {
			buf[2 * idx] = '0';
			buf[2 * idx + 1] = ' ';
		}
	}
	buf[8] = 0;
	return 9;
}

static DEVICE_ATTR_RO(gmsl_links);

static int gmsl_camera_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep = { };
	const struct gmsl_board_desc *board;
	struct gmsl_camera *camera;
	struct gmsl_dev *gmsl;
	int ret;

	dev_info(dev, "max96724-imx390: probing...\n");

	board = i2c_get_match_data(client);
	if (!board || !board->num_links) {
		dev_err(dev, "no matching board descriptor\n");
		return -ENODEV;
	}

	camera = devm_kzalloc(dev, sizeof(*camera), GFP_KERNEL);
	if (!camera)
		return -ENOMEM;

	gmsl = &camera->gmsl;
	gmsl->client = client;
	gmsl->dev = dev;
	mutex_init(&camera->lock);

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		v4l2_fwnode_endpoint_free(&ep);
		return ret;
	}

	gmsl->mbus_config.type = ep.bus_type;
	gmsl->mbus_config.link_freq = ep.nr_of_link_frequencies ?
					      ep.link_frequencies[0] :
					      board->links[0].link_freq;
	gmsl->mbus_config.bus.mipi_csi2 = ep.bus.mipi_csi2;
	v4l2_fwnode_endpoint_free(&ep);

	if (gmsl->mbus_config.type != V4L2_MBUS_PARALLEL &&
	    gmsl->mbus_config.type != V4L2_MBUS_CSI2_DPHY &&
	    gmsl->mbus_config.type != V4L2_MBUS_BT656) {
		dev_err(dev, "Unsupported bus type %d\n",
			gmsl->mbus_config.type);
		return -EINVAL;
	}

	camera->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(camera->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(camera->xclk);
	}

	camera->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(camera->pwdn_gpio))
		return PTR_ERR(camera->pwdn_gpio);

	camera->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(camera->reset_gpio))
		return PTR_ERR(camera->reset_gpio);

	ret = gmsl_core_set_board(gmsl, board);
	if (ret) {
		dev_err(dev, "failed to set board descriptor\n");
		return ret;
	}

	v4l2_i2c_subdev_init(&camera->sd, client, &gmsl_camera_subdev_ops);
	v4l2_set_subdevdata(&camera->sd, camera);
	camera->sd.internal_ops = &gmsl_camera_internal_ops;

	camera->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	camera->pad[0].flags = MEDIA_PAD_FL_SOURCE;
	camera->pad[1].flags = MEDIA_PAD_FL_SOURCE;
	camera->pad[2].flags = MEDIA_PAD_FL_SOURCE;
	camera->pad[3].flags = MEDIA_PAD_FL_SOURCE;
	camera->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&camera->sd.entity, GMSL_MAX_LINKS,
				     camera->pad);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(&camera->sd);
	if (ret)
		goto entity_cleanup;

	ret = gmsl_camera_get_regulators(camera);
	if (ret)
		goto entity_cleanup;

	ret = gmsl_core_hw_init(gmsl);
	if (ret) {
		dev_err(dev, "failed to setup deserializer\n");
		goto entity_cleanup;
	}

	ret = gmsl_camera_sensor_resume(dev);
	if (ret) {
		dev_err(dev, "failed to power on\n");
		goto entity_cleanup;
	}

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&camera->sd);
	if (ret)
		goto err_pm_runtime;

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	device_create_file(dev, &dev_attr_gmsl_links);

	dev_info(dev, "max96724-imx390: probe success!\n");

	return 0;

err_pm_runtime:
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	gmsl_camera_sensor_suspend(dev);
entity_cleanup:
	v4l2_subdev_cleanup(&camera->sd);
	media_entity_cleanup(&camera->sd.entity);
	mutex_destroy(&camera->lock);
	return ret;
}

static void gmsl_camera_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gmsl_camera *camera = to_gmsl_camera(sd);
	struct device *dev = &client->dev;

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		gmsl_camera_sensor_suspend(dev);
	pm_runtime_set_suspended(dev);

	v4l2_async_unregister_subdev(&camera->sd);
	v4l2_subdev_cleanup(&camera->sd);
	media_entity_cleanup(&camera->sd.entity);
	mutex_destroy(&camera->lock);
}

static const struct dev_pm_ops gmsl_camera_pm_ops = {
	SET_RUNTIME_PM_OPS(gmsl_camera_sensor_suspend,
			   gmsl_camera_sensor_resume, NULL)
};

static const struct i2c_device_id gmsl_camera_id[] = {
	{ "max96724_imx390", (kernel_ulong_t)&max96724_imx390_board },
	{ "max96724_ar0820", (kernel_ulong_t)&max96724_ar0820_board },
	{ "max96724_imx490", (kernel_ulong_t)&max96724_imx490_board },
	{ "max96724_isx031", (kernel_ulong_t)&max96724_isx031_board },
	{ "max96724_sg2m820", (kernel_ulong_t)&max96724_sg2m820_board },
	{},
};
MODULE_DEVICE_TABLE(i2c, gmsl_camera_id);

static const struct of_device_id gmsl_camera_dt_ids[] = {
	{ .compatible = "zuojisi,max96724-imx390", .data = &max96724_imx390_board },
	{ .compatible = "zuojisi,max96724-ar0820", .data = &max96724_ar0820_board },
	{ .compatible = "zuojisi,max96724-imx490", .data = &max96724_imx490_board },
	{ .compatible = "zuojisi,max96724-isx031", .data = &max96724_isx031_board },
	{ .compatible = "zuojisi,max96724-sg2m820", .data = &max96724_sg2m820_board },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gmsl_camera_dt_ids);

static struct i2c_driver gmsl_camera_i2c_driver = {
	.driver = {
		.name = "max96724_imx390",
		.of_match_table = gmsl_camera_dt_ids,
		.pm = &gmsl_camera_pm_ops,
	},
	.id_table = gmsl_camera_id,
	.probe = gmsl_camera_probe,
	.remove = gmsl_camera_remove,
};

module_i2c_driver(gmsl_camera_i2c_driver);

MODULE_DESCRIPTION("MAX96724 + MAX9295A GMSL2 camera driver (IMX390/AR0820/IMX490/ISX031/SG2M820)");
MODULE_LICENSE("GPL");
