// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight driver for the Kinetic KTD3137
 */

#include <linux/leds.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define DEFAULT_BRIGHTNESS 2047
#define MAX_BRIGHTNESS 2047
#define REG_MAX 0x0A

#define REG_DEV_ID				0x00
#define REG_SW_RESET			0x01
#define	REG_MODE				0x02
#define REG_CONTROL				0x03
#define REG_RATIO_LSB			0x04
#define REG_RATIO_MSB			0x05
#define REG_PWM					0x06
#define REG_RAMP_ON				0x07
#define REG_TRANS_RAMP			0x08
#define REG_FLASH_SETTING		0x09
#define REG_STATUS				0x0A

#define BIT_CH3_FAULT			BIT(7)
#define BIT_CH2_FAULT			BIT(6)
#define BIT_CH1_FAULT			BIT(5)
#define BIT_FLASH_TIMEOUT		BIT(4)
#define BIT_OVP					BIT(3)
#define BIT_UVLO				BIT(2)
#define BIT_OCP					BIT(1)
#define BIT_THERMAL_SHUTDOWN	BIT(0)

#define RESET_CONDITION_BITS		\
	(BIT_CH3_FAULT | BIT_CH2_FAULT | BIT_CH1_FAULT | BIT_OVP | BIT_OCP)

struct ktd3137 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct led_classdev cdev_flash;
	bool led_on;

	bool use_pwm;
	bool use_lsb;
	bool is_linear;

	u32 pwm_period;
	u32 led_current;
	u32 ramp_on_timing;
	u32 ramp_off_timing;
	u32 pwm_transition_dimming;
	u32 i2c_transition_dimming;
	u32 channel;
	u32 ovp_level;
	u32 switching_freq;
	u32 inductor_current;
	u32 flash_timeout;
};

static const struct regmap_config ktd3137_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_MAX,
};

static int ktd3137_read(struct ktd3137 *ktd, unsigned int reg,
			 unsigned int *val)
{
	return regmap_read(ktd->regmap, reg, val);
}

static int ktd3137_write(struct ktd3137 *ktd, unsigned int reg,
			 unsigned int val)
{
	return regmap_write(ktd->regmap, reg, val);
}

static int ktd3137_update_bits(struct ktd3137 *ktd, unsigned int reg,
			       unsigned int mask, unsigned int val)
{
	return regmap_update_bits(ktd->regmap, reg, mask, val);
}


static int ktd3137_init(struct ktd3137 *ktd)
{
	unsigned int val = 0;

	ktd->use_pwm = of_property_read_bool(ktd->client->dev.of_node, "ktd,pwm");
	ktd->use_lsb = of_property_read_bool(ktd->client->dev.of_node, "ktd,lsb");

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,pwm-frequency", &ktd->pwm_period)) {
		dev_err(&ktd->client->dev, "ktd,pwm-frequency is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,led-current", &ktd->led_current)) {
		dev_err(&ktd->client->dev, "ktd,led-current is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,ramp-on-timing", &ktd->ramp_on_timing)) {
		dev_err(&ktd->client->dev, "ktd,ramp-on-timing is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,ramp-off-timing", &ktd->ramp_off_timing)) {
		dev_err(&ktd->client->dev, "ktd,ramp-off-timing is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,pwm-transition", &ktd->pwm_transition_dimming)) {
		dev_err(&ktd->client->dev, "ktd,pwm-transition is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,i2c-transition", &ktd->i2c_transition_dimming)) {
		dev_err(&ktd->client->dev, "ktd,i2c-transition is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,channel", &ktd->channel)) {
		dev_err(&ktd->client->dev, "ktd,channel is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,ovp-level", &ktd->ovp_level)) {
		dev_err(&ktd->client->dev, "ktd,ovp-level is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,switching-frequency", &ktd->switching_freq)) {
		dev_err(&ktd->client->dev, "ktd,switching-frequency is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,inductor-current", &ktd->inductor_current)) {
		dev_err(&ktd->client->dev, "ktd,inductor-current is missing\n");
		return -EINVAL;
	}

	if (of_property_read_u32(ktd->client->dev.of_node, "ktd,flash-timeout", &ktd->flash_timeout)) {
		dev_err(&ktd->client->dev, "ktd,flash-timeout is missing\n");
		return -EINVAL;
	}

	ktd->is_linear = of_property_read_bool(ktd->client->dev.of_node, "ktd,linear");

	if (ktd->ovp_level == 32)
		val = 0x20;
	if (ktd->inductor_current == 2600)
		val |= 0x08;
	if (ktd->switching_freq == 1000)
		val |= 0x40;
	if (ktd->is_linear)
		val |= 0x06;
	if (ktd3137_write(ktd, REG_CONTROL, val)) {
		dev_err(&ktd->client->dev, "failed to setup control reg\n");
		return -EINVAL;
	}

	val = 0;
	switch (ktd->channel) {
		case 0:
			val = ktd3137_write(ktd, REG_PWM, 0x18);
			break;
		case 2:
			val = ktd3137_update_bits(ktd, REG_PWM, 0x9B, 0x1B);
			break;
		case 3:
			val = ktd3137_update_bits(ktd, REG_PWM, 0x9F, 0x9F);
			break;
		default:
			dev_err(&ktd->client->dev, "invalid ktd,channel value\n");
	}

	if (val) {
		dev_err(&ktd->client->dev, "failed to setup pwm reg\n");
		return val;
	}

	if (ktd->use_pwm)
		val = ktd3137_update_bits(ktd, REG_PWM, 0x80, 0x80);
	else
		val = ktd3137_update_bits(ktd, REG_PWM, 0x9B, 0x1B);

	if (val) {
		dev_err(&ktd->client->dev, "failed to setup pwm mode\n");
		return val;
	}

	/// XXX: i assume ramp on and off are 0
	val = ktd3137_update_bits(ktd, REG_RAMP_ON, 0xF0, 0x00); // on
	if (val) {
		dev_err(&ktd->client->dev, "failed to setup ramp_on reg\n");
		return val;
	}
	val = ktd3137_update_bits(ktd, REG_RAMP_ON, 0xF0, 0x00); // off
	if (val) {
		dev_err(&ktd->client->dev, "failed to setup ramp_on reg\n");
		return val;
	}
	/// XXX: i assume i2c and pwm transition dimming is 0
	val = ktd3137_update_bits(ktd, REG_TRANS_RAMP, 0x70, 0x00); // pwm
	if (val) {
		dev_err(&ktd->client->dev, "failed to setup ramp reg\n");
		return val;
	}
	val = ktd3137_update_bits(ktd, REG_TRANS_RAMP, 0x0f, 0x00); // i2c
	if (val) {
		dev_err(&ktd->client->dev, "failed to setup ramp reg\n");
		return val;
	}

	val = ktd3137_update_bits(ktd, REG_MODE, 0xF8, ktd->led_current);
	if (val) {
		dev_err(&ktd->client->dev, "failed to setup mode reg\n");
		return val;
	}

	val = ktd3137_write(ktd, REG_TRANS_RAMP, 0x03);
	if (val) {
		dev_err(&ktd->client->dev, "failed to setup mode reg\n");
		return val;
	}

	return 0;
}

static int ktd3137_check_status(struct ktd3137 *ktd) {
	u32 val = 0;

	ktd3137_read(ktd, REG_STATUS, &val);
	if (val) {
		if (val & RESET_CONDITION_BITS) {
			val = ktd3137_init(ktd);
			if (val) {
				dev_err(&ktd->client->dev, "failed to reinit the chip\n");
				return val;
			}
		}
	}

	return 0;
}

static int ktd3137_backlight_update_status(struct backlight_device *backlight_dev)
{
	struct ktd3137 *ktd = bl_get_data(backlight_dev);
	unsigned int brightness = backlight_get_brightness(backlight_dev);

	if (!ktd->led_on && brightness > 0) {
		ktd3137_update_bits(ktd, REG_MODE, 0x01, 0x01);
		ktd->led_on = true;
	} else if (brightness == 0) {
		ktd3137_update_bits(ktd, REG_MODE, 0x01, 0x00);
		ktd3137_write(ktd, REG_RAMP_ON, 0x44);
		ktd3137_write(ktd, REG_MODE, 0x98);
		ktd->led_on = false;
	}

	if (ktd->is_linear)
		ktd3137_update_bits(ktd, REG_CONTROL, 0x02, 0x02);

	if (ktd->use_lsb) {
		ktd3137_update_bits(ktd, REG_RATIO_LSB, 0x07, brightness);
		ktd3137_update_bits(ktd, REG_RATIO_MSB, 0xff, brightness >> 3);
	} else {
		panic("TODO: only lsb mode is supported");
	}

	ktd3137_check_status(ktd);

	return 0;
}

static const struct backlight_ops ktd3137_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = ktd3137_backlight_update_status,
};

static void ktd3137_flash_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct ktd3137 *ktd;
	u32 val;

	ktd = container_of(cdev, struct ktd3137, cdev_flash);

	if (!brightness)
		return;

	if (ktd->flash_timeout < 100)
		val = 0x00;
	else if (ktd->flash_timeout > 1500)
		val = 0x0f;
	else
		val = ktd->flash_timeout / 100;

	val = (val << 4) | brightness;

	if (ktd3137_write(ktd, REG_FLASH_SETTING, val))
		goto i2c_err;

	if (ktd3137_update_bits(ktd, REG_MODE, 0x02, 0x02))
		goto i2c_err;

	if (ktd3137_check_status(ktd))
		dev_err(&ktd->client->dev, "status check failed\n");

	return;
i2c_err:
	dev_err(&ktd->client->dev, "i2c write failed\n");
}

static int ktd3137_probe(struct i2c_client *client)
{
	struct backlight_device *backlight_dev;
	struct backlight_properties props;
	struct ktd3137 *ktd;
	int ret = 0, val = 0;

	ktd = devm_kzalloc(&client->dev, sizeof(*ktd), GFP_KERNEL);
	if (!ktd)
		return -ENOMEM;

	ktd->client = client;
	ktd->regmap = devm_regmap_init_i2c(client, &ktd3137_regmap_config);
	if (IS_ERR(ktd->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(ktd->regmap), "failed to init regmap\n");

	ret = ktd3137_read(ktd, REG_DEV_ID, &val);
	if (ret)
	  return dev_err_probe(&client->dev, ret, "failed to get chip id\n");
	dev_err(&client->dev, "chip id: %d\n", val);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = MAX_BRIGHTNESS;
	props.brightness = DEFAULT_BRIGHTNESS;
	props.scale = BACKLIGHT_SCALE_LINEAR;

	backlight_dev = devm_backlight_device_register(&client->dev, "ktd3137-backlight",
					&client->dev, ktd, &ktd3137_backlight_ops, &props);
	if (IS_ERR(backlight_dev))
		return dev_err_probe(&client->dev, PTR_ERR(backlight_dev),
				"failed to register backlight device\n");

	ktd->cdev_flash.name = "ktd3137_flash";
	ktd->cdev_flash.max_brightness = 16;
	ktd->cdev_flash.brightness_set = ktd3137_flash_brightness_set;

	ret = devm_led_classdev_register(&client->dev, &ktd->cdev_flash);
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to register flash\n");

	ret = ktd3137_init(ktd);
	if (ret)
		return ret;

	ktd3137_check_status(ktd);

	i2c_set_clientdata(client, backlight_dev);
	backlight_update_status(backlight_dev);

	return 0;
}

static void ktd3137_remove(struct i2c_client *client)
{
	struct backlight_device *backlight_dev = i2c_get_clientdata(client);
	backlight_dev->props.brightness = 0;
	backlight_update_status(backlight_dev);
}

static const struct i2c_device_id ktd3137_ids[] = {
	{ "ktd3137" },
	{}
};
MODULE_DEVICE_TABLE(i2c, ktd3137_ids);

static const struct of_device_id ktd3137_match_table[] = {
	{ .compatible = "kinetic,ktd3137" },
	{},
};
MODULE_DEVICE_TABLE(of, ktd3137_match_table);

static struct i2c_driver ktd3137_driver = {
	.driver = {
		.name = "ktd3137",
		.of_match_table = ktd3137_match_table,
	},
	.probe = ktd3137_probe,
	.remove = ktd3137_remove,
	.id_table = ktd3137_ids,
};

module_i2c_driver(ktd3137_driver);

MODULE_DESCRIPTION("Kinetic ktd3137 Backlight Driver");
MODULE_AUTHOR("Me, myself, and I");
MODULE_LICENSE("GPL");

