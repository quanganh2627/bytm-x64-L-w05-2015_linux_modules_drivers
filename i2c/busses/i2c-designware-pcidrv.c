/*
 * Synopsys DesignWare I2C adapter driver (master only).
 *
 * Based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 * Copyright (C) 2011 Intel corporation.
 *
 * ----------------------------------------------------------------------------
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * ----------------------------------------------------------------------------
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/acpi.h>
#include "i2c-designware-core.h"

#define DRIVER_NAME "i2c-designware-pci"
#define DW_I2C_STATIC_BUS_NUM	10

#define DW_STD_SPEED	100000
#define DW_FAST_SPEED	400000
#define DW_HIGH_SPEED	3400000

#ifdef CONFIG_ACPI
struct i2c_dw_board_info {
	struct i2c_adapter *adap;
	struct i2c_board_info info;
};

static int i2c_dw_find_irq(struct acpi_resource *ares, void *data)
{
	struct i2c_dw_board_info *dwinfo = data;

	if (dwinfo->info.irq < 0) {
		struct resource r;

		if (acpi_dev_resource_interrupt(ares, 0, &r))
			dwinfo->info.irq = r.start;
	}

	/* Tell the ACPI core to skip this resource */
	return 1;
}

static int i2c_dw_find_slaves(struct acpi_resource *ares, void *data)
{
	struct i2c_dw_board_info *dwinfo = data;
	struct device *dev = &dwinfo->adap->dev;
	struct dw_i2c_dev *i2c = i2c_get_adapdata(dwinfo->adap);
	unsigned int connection_speed;

	if (ares->type == ACPI_RESOURCE_TYPE_SERIAL_BUS) {
		struct acpi_resource_i2c_serialbus *sb;

		sb = &ares->data.i2c_serial_bus;
		if (sb->type == ACPI_RESOURCE_SERIAL_TYPE_I2C) {
			connection_speed = sb->connection_speed;
			if (connection_speed == DW_STD_SPEED) {
				i2c->master_cfg &= ~DW_IC_SPEED_MASK;
				i2c->master_cfg |= DW_IC_CON_SPEED_STD;
			} else if (connection_speed == DW_FAST_SPEED) {
				i2c->master_cfg &= ~DW_IC_SPEED_MASK;
				i2c->master_cfg |= DW_IC_CON_SPEED_FAST;
			} else if (connection_speed == DW_HIGH_SPEED) {
				i2c->master_cfg &= ~DW_IC_SPEED_MASK;
				i2c->master_cfg |= DW_IC_CON_SPEED_HIGH;
			}

			down(&i2c->lock);
			i2c_dw_init(i2c);
			up(&i2c->lock);

			dev_info(dev, "I2C speed get from acpi is %dKHz\n",
				connection_speed/1000);

			dwinfo->info.addr = sb->slave_address;
			if (sb->access_mode == ACPI_I2C_10BIT_MODE)
				dwinfo->info.flags |= I2C_CLIENT_TEN;
			dev_info(dev, "\t\tslave_addr 0x%x, irq %d\n",
				dwinfo->info.addr, dwinfo->info.irq);
			if (!i2c_new_device(dwinfo->adap, &dwinfo->info))
				dev_err(dev, "failed to add %s\n",
					dwinfo->info.type);
		}
	}

	/* Tell the ACPI core to skip this resource */
	return 1;
}

static acpi_status i2c_dw_add_device(acpi_handle handle, u32 level,
				       void *data, void **return_value)
{
	struct i2c_dw_board_info *dwinfo = data;
	struct device *dev = &dwinfo->adap->dev;
	struct list_head resource_list;
	struct acpi_device *adev;
	int ret;

	dev_info(dev, "\tCheck next device ...");
	ret = acpi_bus_get_device(handle, &adev);
	if (ret) {
		dev_info(dev, "\t err %d\n", ret);
		return AE_OK;
	}
	dev_info(dev, "\t%s\n", dev_name(&adev->dev));
	if (acpi_bus_get_status(adev) || !adev->status.present) {
		dev_err(dev, "\t\terror, present %d\n", adev->status.present);
		return AE_OK;
	}

	dwinfo->info.acpi_node.handle = handle;
	dwinfo->info.irq = -1;
	strlcpy(dwinfo->info.type, dev_name(&adev->dev),
			sizeof(dwinfo->info.type));

	INIT_LIST_HEAD(&resource_list);
	acpi_dev_get_resources(adev, &resource_list,
				     i2c_dw_find_irq, dwinfo);
	acpi_dev_get_resources(adev, &resource_list,
				     i2c_dw_find_slaves, dwinfo);
	acpi_dev_free_resource_list(&resource_list);

	return AE_OK;
}

static void i2c_dw_scan_devices(struct i2c_adapter *adapter, char *acpi_name)
{
	acpi_handle handle;
	acpi_status status;
	struct i2c_dw_board_info dw_info;
	struct device *dev = &adapter->dev;

	dev_err(dev, "Scan devices on i2c-%d\n", adapter->nr);
	memset(&dw_info, 0, sizeof(dw_info));
	dw_info.adap = adapter;
	acpi_get_handle(NULL, acpi_name, &handle);
	acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,
				     i2c_dw_add_device, NULL,
				     &dw_info, NULL);
}
#else
static void i2c_dw_scan_devices(struct i2c_adapter *adapter, char *acpi_name) {}
#endif

static int i2c_dw_pci_suspend(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct dw_i2c_dev *i2c = pci_get_drvdata(pdev);

	dev_dbg(dev, "suspend called\n");

	return i2c_dw_suspend(i2c, false);
}

static int i2c_dw_pci_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct dw_i2c_dev *i2c = pci_get_drvdata(pdev);
	int err;

	dev_dbg(dev, "runtime suspend called\n");
	i2c_dw_suspend(i2c, true);

	err = pci_save_state(pdev);
	if (err) {
		dev_err(&pdev->dev, "pci_save_state failed\n");
		return err;
	}

	err = pci_set_power_state(pdev, PCI_D3hot);
	if (err) {
		dev_err(&pdev->dev, "pci_set_power_state failed\n");
		return err;
	}

	return 0;
}

static int i2c_dw_pci_resume(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct dw_i2c_dev *i2c = pci_get_drvdata(pdev);

	dev_dbg(dev, "resume called\n");
	return i2c_dw_resume(i2c, false);
}

static int i2c_dw_pci_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = container_of(dev, struct pci_dev, dev);
	struct dw_i2c_dev *i2c = pci_get_drvdata(pdev);
	int err;

	dev_dbg(dev, "runtime resume called\n");
	err = pci_set_power_state(pdev, PCI_D0);
	if (err) {
		dev_err(&pdev->dev, "pci_set_power_state() failed\n");
		return err;
	}
	pci_restore_state(pdev);
	i2c_dw_resume(i2c, true);

	return 0;
}

static const struct dev_pm_ops i2c_dw_pm_ops = {
	.suspend_late = i2c_dw_pci_suspend,
	.resume_early = i2c_dw_pci_resume,
	SET_RUNTIME_PM_OPS(i2c_dw_pci_runtime_suspend,
			   i2c_dw_pci_runtime_resume,
			   NULL)
};

static int i2c_dw_pci_probe(struct pci_dev *pdev,
const struct pci_device_id *id)
{
	struct dw_i2c_dev *dev;
	unsigned long start, len;
	int r;
	int bus_idx;
	static int bus_num;

	bus_idx = id->driver_data + bus_num;
	bus_num++;

	r = pci_enable_device(pdev);
	if (r) {
		dev_err(&pdev->dev, "Failed to enable I2C PCI device (%d)\n",
			r);
		return r;
	}

	/* Determine the address of the I2C area */
	start = pci_resource_start(pdev, 0);
	len = pci_resource_len(pdev, 0);
	if (!start || len == 0) {
		dev_err(&pdev->dev, "base address not set\n");
		return -ENODEV;
	}

	r = pci_request_region(pdev, 0, DRIVER_NAME);
	if (r) {
		dev_err(&pdev->dev, "failed to request I2C region "
			"0x%lx-0x%lx\n", start,
			(unsigned long)pci_resource_end(pdev, 0));
		return r;
	}

	dev = i2c_dw_setup(&pdev->dev, bus_idx, start, len, pdev->irq);
	if (IS_ERR(dev)) {
		pci_release_region(pdev, 0);
		dev_err(&pdev->dev, "failed to setup i2c\n");
		return -EINVAL;
 	}

	pci_set_drvdata(pdev, dev);

	if (dev->controller->acpi_name)
		i2c_dw_scan_devices(&dev->adapter, dev->controller->acpi_name);

	pm_runtime_set_autosuspend_delay(&pdev->dev, 50);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_allow(&pdev->dev);

	return 0;
}

static void i2c_dw_pci_remove(struct pci_dev *pdev)
{
	struct dw_i2c_dev *dev = pci_get_drvdata(pdev);

	pm_runtime_forbid(&pdev->dev);
	i2c_dw_free(&pdev->dev, dev);
	pci_set_drvdata(pdev, NULL);
	pci_release_region(pdev, 0);
}

/* work with hotplug and coldplug */
MODULE_ALIAS("i2c_designware-pci");

DEFINE_PCI_DEVICE_TABLE(i2c_designware_pci_ids) = {
	/* Moorestown */
	{ PCI_VDEVICE(INTEL, 0x0802), moorestown_0 },
	{ PCI_VDEVICE(INTEL, 0x0803), moorestown_0 },
	{ PCI_VDEVICE(INTEL, 0x0804), moorestown_0 },
	/* Medfield */
	{ PCI_VDEVICE(INTEL, 0x0817), medfield_0 },
	{ PCI_VDEVICE(INTEL, 0x0818), medfield_0 },
	{ PCI_VDEVICE(INTEL, 0x0819), medfield_0 },
	{ PCI_VDEVICE(INTEL, 0x082C), medfield_0 },
	{ PCI_VDEVICE(INTEL, 0x082D), medfield_0 },
	{ PCI_VDEVICE(INTEL, 0x082E), medfield_0 },
	/* Cloverview */
	{ PCI_VDEVICE(INTEL, 0x08E2), cloverview_0 },
	{ PCI_VDEVICE(INTEL, 0x08E3), cloverview_0 },
	{ PCI_VDEVICE(INTEL, 0x08E4), cloverview_0 },
	{ PCI_VDEVICE(INTEL, 0x08F4), cloverview_0 },
	{ PCI_VDEVICE(INTEL, 0x08F5), cloverview_0 },
	{ PCI_VDEVICE(INTEL, 0x08F6), cloverview_0 },
	/* Merrifield */
	{ PCI_VDEVICE(INTEL, 0x1195), merrifield_0 },
	{ PCI_VDEVICE(INTEL, 0x1196), merrifield_0 },
	/* Valleyview 2 */
	{ PCI_VDEVICE(INTEL, 0x0F41), valleyview_0 },
	{ PCI_VDEVICE(INTEL, 0x0F42), valleyview_0 },
	{ PCI_VDEVICE(INTEL, 0x0F43), valleyview_0 },
	{ PCI_VDEVICE(INTEL, 0x0F44), valleyview_0 },
	{ PCI_VDEVICE(INTEL, 0x0F45), valleyview_0 },
	{ PCI_VDEVICE(INTEL, 0x0F46), valleyview_0 },
	{ PCI_VDEVICE(INTEL, 0x0F47), valleyview_0 },
	{ 0,}
};
MODULE_DEVICE_TABLE(pci, i2c_designware_pci_ids);

static struct pci_driver dw_i2c_driver = {
	.name		= DRIVER_NAME,
	.id_table	= i2c_designware_pci_ids,
	.probe		= i2c_dw_pci_probe,
	.remove		= i2c_dw_pci_remove,
	.driver         = {
		.pm     = &i2c_dw_pm_ops,
	},
};

static int __init dw_i2c_init_driver(void)
{
	return  pci_register_driver(&dw_i2c_driver);
}
module_init(dw_i2c_init_driver);

static void __exit dw_i2c_exit_driver(void)
{
	pci_unregister_driver(&dw_i2c_driver);
}
module_exit(dw_i2c_exit_driver);

#ifndef MODULE
static int __init dw_i2c_reserve_static_bus(void)
{
	struct i2c_board_info dummy = {
		I2C_BOARD_INFO("dummy", 0xff),
	};

	i2c_register_board_info(DW_I2C_STATIC_BUS_NUM, &dummy, 1);
	return 0;
}
subsys_initcall(dw_i2c_reserve_static_bus);

static void dw_i2c_pci_final_quirks(struct pci_dev *pdev)
{
	pdev->pm_cap = 0x80;
}

DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0F44,
				dw_i2c_pci_final_quirks);
#endif

MODULE_AUTHOR("Baruch Siach <baruch@tkos.co.il>");
MODULE_DESCRIPTION("Synopsys DesignWare PCI I2C bus adapter");
MODULE_LICENSE("GPL");
