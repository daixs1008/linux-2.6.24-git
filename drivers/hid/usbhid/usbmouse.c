/*
 * $Id: usbmouse.c,v 1.15 2001/12/27 10:37:41 vojtech Exp $
 *
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 *
 *  USB HIDBP Mouse support
 */

/*
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb/input.h>
#include <linux/hid.h>

/*
 * Version Information
 */
#define DRIVER_VERSION "v1.6"
#define DRIVER_AUTHOR "Vojtech Pavlik <vojtech@ucw.cz>"
#define DRIVER_DESC "USB HID Boot Protocol mouse driver"
#define DRIVER_LICENSE "GPL"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE(DRIVER_LICENSE);

struct usb_mouse {
	char name[128];  //名字，一般存储制造商的名称
	char phys[64];
	struct usb_device *usbdev;  //usb 设备模型
	struct input_dev *dev;  //输入设备
	struct urb *irq;  //用于usb 设备通信的urb 模块

	signed char *data;  //usb 鼠标事件的buffer ，存储鼠标的左键，右键，滑轮，坐标事件
	dma_addr_t data_dma;
};

static void usb_mouse_irq(struct urb *urb)
{
	struct usb_mouse *mouse = urb->context;
	signed char *data = mouse->data;
	struct input_dev *dev = mouse->dev;
	int status;

	switch (urb->status) {
	case 0:			/* success */
		break;
	case -ECONNRESET:	/* unlink */
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	/* -EPIPE:  should clear the halt */
	default:		/* error */
		goto resubmit;
	}

	input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
	input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
	input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);
	input_report_key(dev, BTN_SIDE,   data[0] & 0x08);
	input_report_key(dev, BTN_EXTRA,  data[0] & 0x10);

	input_report_rel(dev, REL_X,     data[1]);
	input_report_rel(dev, REL_Y,     data[2]);
	input_report_rel(dev, REL_WHEEL, data[3]);

	input_sync(dev);  //提交鼠标数据并同步
resubmit:
	status = usb_submit_urb (urb, GFP_ATOMIC);  //提交urb 告诉主机有数据可以读
	if (status)
		err ("can't resubmit intr, %s-%s/input0, status %d",
				mouse->usbdev->bus->bus_name,
				mouse->usbdev->devpath, status);
}

static int usb_mouse_open(struct input_dev *dev)
{
	struct usb_mouse *mouse = input_get_drvdata(dev);

	mouse->irq->dev = mouse->usbdev;
	if (usb_submit_urb(mouse->irq, GFP_KERNEL))
		return -EIO;

	return 0;
}

static void usb_mouse_close(struct input_dev *dev)
{
	struct usb_mouse *mouse = input_get_drvdata(dev);

	usb_kill_urb(mouse->irq);
}

static int usb_mouse_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_mouse *mouse;
	struct input_dev *input_dev;
	int pipe, maxp;
	int error = -ENOMEM;

	interface = intf->cur_altsetting;  //获取当前usb设备的接口描述符

	if (interface->desc.bNumEndpoints != 1)  //判断鼠标设备只有一个端点（除了端点0）
		return -ENODEV;

	endpoint = &interface->endpoint[0].desc;  //获取当前端点描述符，此处 endpoint[0]，并不是第0个端点。而是除端点0 以外的第0 个端点。
	if (!usb_endpoint_is_int_in(endpoint))  //如果不是中断输入类型端点，出错返回。  根据usb_endpoint_descriptor->bmAttributes 判断
		return -ENODEV;
	//创建中断输入管道，鼠标属于中断控制，
	pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);  //usb 设备数据传输的源  （管道传输）  //整数pipe-包含端点的类型，设备地址和端点地址，端点的方向
	//返回该端点能够传输的最大包长度，鼠标的返回的最大数据包长度为4个字节
	//初始化urb 的时候会用到这个长度，缓冲区的长度要依照maxp 来确定，最大不能超过8
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	mouse = kzalloc(sizeof(struct usb_mouse), GFP_KERNEL);  //为mouse 申请内存，mouse 结构的主要作用是赋值给usb_interface中的一个属性，这个属性存储用户需要的数据
	input_dev = input_allocate_device();//创建input设备
	if (!mouse || !input_dev)
		goto fail1;

	//为urb 的传输申请内存，data指向该地址空间，初始化urb缓冲区，
	mouse->data = usb_buffer_alloc(dev, 8, GFP_ATOMIC, &mouse->data_dma);  //usb 设备数据传输的目的
	if (!mouse->data)
		goto fail1;

	mouse->irq = usb_alloc_urb(0, GFP_KERNEL);  //分配一个urb ，usb的传输单元  -mouse->irq，并不是真正的中断，usb数据是查询方式传输
	if (!mouse->irq)
		goto fail2;

	mouse->usbdev = dev;
	mouse->dev = input_dev;

	if (dev->manufacturer)
		strlcpy(mouse->name, dev->manufacturer, sizeof(mouse->name));

	if (dev->product) {
		if (dev->manufacturer)
			strlcat(mouse->name, " ", sizeof(mouse->name));
		strlcat(mouse->name, dev->product, sizeof(mouse->name));
	}

	if (!strlen(mouse->name))
		snprintf(mouse->name, sizeof(mouse->name),
			 "USB HIDBP Mouse %04x:%04x",
			 le16_to_cpu(dev->descriptor.idVendor),
			 le16_to_cpu(dev->descriptor.idProduct));

	usb_make_path(dev, mouse->phys, sizeof(mouse->phys));
	strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

	input_dev->name = mouse->name;
	input_dev->phys = mouse->phys;
	//从设备中获取总线类型，设备id，厂商id，版本号，设置父设备，
	usb_to_input_id(dev, &input_dev->id);
	input_dev->dev.parent = &intf->dev;

	//设置输入事件所支持的事件信息  
	//支持坐标和事件
	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	//记录支持的按键值
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) |
		BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
	//记录支持的相对坐标为鼠标移动坐标和滑轮的坐标
	input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] |= BIT_MASK(BTN_SIDE) |
		BIT_MASK(BTN_EXTRA);
	input_dev->relbit[0] |= BIT_MASK(REL_WHEEL);

	//将mouse传入 input_dev，方面通过input_dev获取全部的mouse信息
	input_set_drvdata(input_dev, mouse);

	//设置输入设备的 open和close 函数
	input_dev->open = usb_mouse_open;
	input_dev->close = usb_mouse_close;

	//填充urb模块，mouse作为上下文被设置，另外usb_mouse_irq 被作为回调函数   
	//当usb mouse有事件产生时，回调函数被调用
	usb_fill_int_urb(mouse->irq, dev, pipe, mouse->data,  //使用3要素填充 urb
			 (maxp > 8 ? 8 : maxp),
			 usb_mouse_irq, mouse, endpoint->bInterval);  //endpoint->bInterval---usb数据传输查询频率
	//mouse->irq 就是urb，如下设置DMA传输相关，跟据flag为 URB_NO_TRANSFER_DMA_MAP 时，判断优先使用 transfer_dma  或 transfer buffer 
	mouse->irq->transfer_dma = mouse->data_dma;
	mouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	error = input_register_device(mouse->dev);
	if (error)
		goto fail3;

	usb_set_intfdata(intf, mouse);  //注册mouse 到 usb_interface 中
	return 0;

fail3:	
	usb_free_urb(mouse->irq);
fail2:	
	usb_buffer_free(dev, 8, mouse->data, mouse->data_dma);
fail1:	
	input_free_device(input_dev);
	kfree(mouse);
	return error;
}

static void usb_mouse_disconnect(struct usb_interface *intf)
{
	struct usb_mouse *mouse = usb_get_intfdata (intf);

	usb_set_intfdata(intf, NULL);
	if (mouse) {
		usb_kill_urb(mouse->irq);
		input_unregister_device(mouse->dev);
		usb_free_urb(mouse->irq);
		usb_buffer_free(interface_to_usbdev(intf), 8, mouse->data, mouse->data_dma);
		kfree(mouse);
	}
}

static struct usb_device_id usb_mouse_id_table [] = {
	{ USB_INTERFACE_INFO(USB_INTERFACE_CLASS_HID, USB_INTERFACE_SUBCLASS_BOOT,
		USB_INTERFACE_PROTOCOL_MOUSE) },
	{ }	/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, usb_mouse_id_table);

static struct usb_driver usb_mouse_driver = {
	.name		= "usbmouse",
	.probe		= usb_mouse_probe,
	.disconnect	= usb_mouse_disconnect,
	.id_table	= usb_mouse_id_table,
};

static int __init usb_mouse_init(void)
{
	int retval = usb_register(&usb_mouse_driver);
	if (retval == 0)
		info(DRIVER_VERSION ":" DRIVER_DESC);
	return retval;
}

static void __exit usb_mouse_exit(void)
{
	usb_deregister(&usb_mouse_driver);
}

module_init(usb_mouse_init);
module_exit(usb_mouse_exit);
