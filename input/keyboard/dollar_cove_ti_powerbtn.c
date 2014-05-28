/*
 * Power button driver for dollar cove
 *
 * Copyright (C) 2014 Intel Corp
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/mfd/intel_mid_pmic.h>

#define DC_TI_IRQ_MASK_REG	0x02
#define IRQ_MASK_PWRBTN		(1 << 0)

#define DC_TI_SIRQ_REG		0x3
#define SIRQ_PWRBTN_REL		(1 << 0)

#define DRIVER_NAME "dollar_cove_ti_power_button"

static struct input_dev *pb_input;
static int pwrbtn_irq;

static irqreturn_t pb_isr(int irq, void *dev_id)
{
	int ret;

	ret = intel_mid_pmic_readb(DC_TI_SIRQ_REG);
	if (ret < 0) {
		pr_err("[%s] power button SIRQ REG read fail %s\n",
						pb_input->name, ret);
		return IRQ_NONE;
	}

	input_event(pb_input, EV_KEY, KEY_POWER, !(ret & SIRQ_PWRBTN_REL));
	input_sync(pb_input);
	pr_info("[%s] power button %s\n", pb_input->name,
			(ret & SIRQ_PWRBTN_REL) ? "released" : "pressed");

	return IRQ_HANDLED;
}

static int pb_probe(struct platform_device *pdev)
{
	int ret;

	pwrbtn_irq = platform_get_irq(pdev, 0);
	if (pwrbtn_irq < 0) {
		dev_err(&pdev->dev,
			"get irq fail: irq1:%d\n", pwrbtn_irq);
		return -EINVAL;
	}
	pb_input = input_allocate_device();
	if (!pb_input) {
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOMEM;
	}
	pb_input->name = pdev->name;
	pb_input->phys = "power-button/input0";
	pb_input->id.bustype = BUS_HOST;
	pb_input->dev.parent = &pdev->dev;
	input_set_capability(pb_input, EV_KEY, KEY_POWER);
	ret = input_register_device(pb_input);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register input device:%d\n", ret);
		input_free_device(pb_input);
		return ret;
	}

	ret = request_threaded_irq(pwrbtn_irq, NULL, pb_isr,
		IRQF_NO_SUSPEND, DRIVER_NAME, pdev);
	if (ret) {
		dev_err(&pdev->dev,
			"[request irq fail0]irq:%d err:%d\n", pwrbtn_irq, ret);
		input_unregister_device(pb_input);
		return ret;
	}

	return 0;
}

static int pb_remove(struct platform_device *pdev)
{
	free_irq(pwrbtn_irq, NULL);
	input_unregister_device(pb_input);
	return 0;
}

static struct platform_driver pb_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
	},
	.probe	= pb_probe,
	.remove	= pb_remove,
};

static int __init pb_module_init(void)
{
	return platform_driver_register(&pb_driver);
}

static void  pb_module_exit(void)
{
	platform_driver_unregister(&pb_driver);
}

late_initcall(pb_module_init);

module_exit(pb_module_exit);

MODULE_AUTHOR("Ramakrishna Pallala <ramakrishna.pallala@intel.com>");
MODULE_DESCRIPTION("Dollar Cove(TI) Power Button Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
