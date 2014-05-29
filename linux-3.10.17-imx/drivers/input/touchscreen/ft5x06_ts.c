/*
 * Copyright (C) 2014 Watson Xu, <xuhuashan@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>

#define DEVICE_NAME			"ft5x06_ts"
#define MAX_SUPPORT_POINTS		5
#define SCREEN_MAX_X			(800)
#define SCREEN_MAX_Y			(480)

struct ts_finger {
	u16			x;
	u16			y;
	bool			touch;
};

struct touch_data {
	u8			touch_point;
	struct ts_finger	finger[MAX_SUPPORT_POINTS];
};

struct ft5x06_chip_data {
	struct input_dev	*input;
	struct i2c_client	*client;
	int			reset_gpio;
};

static int ft5x06_read_data(struct i2c_client *client,
			    struct touch_data *tdata)
{
	u8 buf[32];
	int i;
	int ret = -1;

	ret = i2c_smbus_read_i2c_block_data(client, 0, 31, buf);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to read I2C data: %d\n", ret);
		return ret;
	}

	tdata->touch_point = buf[2] & 0x07;

	for (i = 0; i < ARRAY_SIZE(tdata->finger); i++)
		tdata->finger[i].touch = false;

	for (i = 0; i < tdata->touch_point; i++) {
		u8 *addr = &buf[3 + i * 6];
		struct ts_finger *finger;
		u8 state, id;
		u16 x, y;

		state = (addr[0] & 0xC0) >> 6;
		x = ((s16)(addr[0] & 0x0F) << 8) | (s16)addr[1];
		y = ((s16)(addr[2] & 0x0F) << 8) | (s16)addr[3];
		id = (s16)(addr[2] & 0xF0) >> 4;

		finger = &tdata->finger[id];
		finger->x = x;
		finger->y = y;
		finger->touch = (state != 1) ? true : false;
	}

	return 0;
}

static void ft5x06_report_value(struct input_dev *input,
				const struct touch_data *tdata)
{
	const struct ts_finger *finger;
	bool touch;
	int i;

	for (i = 0; i < MAX_SUPPORT_POINTS; i++) {
		finger = &tdata->finger[i];

		input_mt_slot(input, i);
		touch = finger->touch;
		input_mt_report_slot_state(input, MT_TOOL_FINGER, touch);
		if (touch) {
			input_report_abs(input, ABS_MT_POSITION_X, finger->x);
			input_report_abs(input, ABS_MT_POSITION_Y, finger->y);
		}
	}
	input_mt_report_pointer_emulation(input, false);
	input_sync(input);
}

static irqreturn_t ft5x06_ts_interrupt(int irq, void *dev_id)
{
	struct ft5x06_chip_data *ft5x06_ts = dev_id;
	struct touch_data touchdata;

	if (!ft5x06_read_data(ft5x06_ts->client, &touchdata))
		ft5x06_report_value(ft5x06_ts->input, &touchdata);

	return IRQ_HANDLED;
}

static int 
ft5x06_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct ft5x06_chip_data *ft5x06_ts;
	struct input_dev *input;
	struct device_node *of_node = client->dev.of_node;
	int err = 0;

	ft5x06_ts = kzalloc(sizeof(*ft5x06_ts), GFP_KERNEL);
	input = input_allocate_device();
	if (!ft5x06_ts || !input) {
		dev_err(&client->dev, "Failed to alocate memory\n");
		err = -ENOMEM;
		goto exit_free_mem;
	}

	ft5x06_ts->client = client;
	ft5x06_ts->input = input;

	input->name = DEVICE_NAME;
	input->id.bustype = BUS_I2C;
	input->dev.parent = &client->dev;

	__set_bit(EV_SYN, input->evbit);
	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_ABS, input->evbit);
	__set_bit(BTN_TOUCH, input->keybit);

	/* Single touch */
	input_set_abs_params(input, ABS_X, 0, SCREEN_MAX_X, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, SCREEN_MAX_Y, 0, 0);

	/* Multi touch */
	input_mt_init_slots(input, MAX_SUPPORT_POINTS, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, SCREEN_MAX_X, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, SCREEN_MAX_Y, 0, 0);

	err = request_threaded_irq(client->irq, NULL, ft5x06_ts_interrupt,
				   IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
				   "ft5x06_ts", ft5x06_ts);
	if (err < 0) {
		dev_err(&client->dev, "Failed to register interrupt\n");
		goto exit_free_mem;
	}

	err = input_register_device(input);
	if (err) {
		dev_err(&client->dev,
			"Failed to register input device: %s\n",
			dev_name(&client->dev));
		goto exit_free_irq;
	}

	i2c_set_clientdata(client, ft5x06_ts);

	return 0;

exit_free_irq:
	free_irq(client->irq, ft5x06_ts);
exit_free_mem:
	input_free_device(input);
	kfree(ft5x06_ts);
	return err;
}

static int ft5x06_ts_remove(struct i2c_client *client)
{
	struct ft5x06_chip_data *ft5x06_ts = i2c_get_clientdata(client);

	free_irq(client->irq, ft5x06_ts);
	input_unregister_device(ft5x06_ts->input);
	kfree(ft5x06_ts);

	return 0;
}

static const struct i2c_device_id ft5x06_ts_id[] = {
	{ DEVICE_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ft5x06_ts_id);

static const struct of_device_id ft5x06_ts_dt_ids[] = {
	{ .compatible = "focaltech,ft5x06", },
	{ /* sentinel */ }
};

static struct i2c_driver ft5x06_ts_driver = {
	.probe		= ft5x06_ts_probe,
	.remove		= ft5x06_ts_remove,
	.id_table	= ft5x06_ts_id,
	.driver	= {
		.name	= DEVICE_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = ft5x06_ts_dt_ids,
	},
};

static int __init ft5x06_ts_init(void)
{
	return i2c_add_driver(&ft5x06_ts_driver);
}

static void __exit ft5x06_ts_exit(void)
{
	i2c_del_driver(&ft5x06_ts_driver);
}

module_init(ft5x06_ts_init);
module_exit(ft5x06_ts_exit);

MODULE_AUTHOR("Watson Xu<xuhuashan@gmail.com>");
MODULE_DESCRIPTION("FocalTech ft5x06 TouchScreen driver");
MODULE_LICENSE("GPL");
