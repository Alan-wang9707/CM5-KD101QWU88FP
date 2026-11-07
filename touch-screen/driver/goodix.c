// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Driver for Goodix Touchscreens
 *
 *  Copyright (c) 2014 Red Hat Inc.
 *  Copyright (c) 2015 K. Merker <merker@debian.org>
 *
 *  This code is based on gt9xx.c authored by andrew@goodix.com:
 *
 *  2010 - 2012 Goodix Technology.
 */


#include <linux/kernel.h>
#include <linux/dmi.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/platform_data/x86/soc.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/unaligned.h>
#include "goodix.h"

#define GOODIX_GPIO_INT_NAME		"irq"
#define GOODIX_GPIO_RST_NAME		"reset"
#define GOODIX_TRACE_ENABLE		1

#if GOODIX_TRACE_ENABLE
#define goodix_trace(dev, fmt, ...) \
	dev_info(dev, "[goodix-trace] " fmt, ##__VA_ARGS__)
#else
#define goodix_trace(dev, fmt, ...) \
	do { \
	} while (0)
#endif

/* Goodix汇顶科技GT9xx系列触控屏驱动通用宏定义 */

/**
 * 驱动支持的最大屏幕参数
 * 限制了驱动能够处理的触控屏物理分辨率上限
 * 4096x4096覆盖了绝大多数消费电子设备的屏幕尺寸
 */
#define GOODIX_MAX_HEIGHT		4096  /* 最大支持的屏幕高度(像素) */
#define GOODIX_MAX_WIDTH		4096  /* 最大支持的屏幕宽度(像素) */

/**
 * 中断与触点数据相关配置
 */
#define GOODIX_INT_TRIGGER		1     /* 中断触发方式默认值: 1=下降沿触发, 0=上升沿触发 */
#define GOODIX_CONTACT_SIZE		8     /* 单个触点数据的标准长度(字节) */
#define GOODIX_MAX_CONTACT_SIZE		9     /* 单个触点数据的最大长度(字节) */
#define GOODIX_MAX_CONTACTS		10    /* 驱动支持的最大同时触摸点数 */

/**
 * 芯片配置文件长度定义
 * 不同型号的GT9xx芯片配置寄存器长度不同
 * 驱动会根据芯片型号读取对应长度的配置数据
 */
#define GOODIX_CONFIG_MIN_LENGTH	186   /* 配置文件最小长度(GT911等早期型号) */
#define GOODIX_CONFIG_911_LENGTH	186   /* GT911/GT9111芯片专用配置长度 */
#define GOODIX_CONFIG_967_LENGTH	228   /* GT967/GT968芯片专用配置长度 */
#define GOODIX_CONFIG_GT9X_LENGTH	240   /* GT9x系列通用配置长度(GT9271/GT928等) */

/**
 * 数据缓冲区状态标志位
 * 用于解析芯片上报的状态寄存器值(0x814E地址)
 */
#define GOODIX_BUFFER_STATUS_READY	BIT(7) /* 第7位: 数据缓冲区就绪, 有新的触摸数据 */
#define GOODIX_HAVE_KEY			BIT(4) /* 第4位: 有物理按键事件(如Home键/返回键) */
#define GOODIX_BUFFER_STATUS_TIMEOUT	20    /* 读取数据缓冲区的超时时间(毫秒) */

/**
 * 配置数组偏移量定义
 * 这些偏移量指向配置数组中特定参数的位置
 * 驱动通过这些偏移量读取/修改芯片的关键配置
 */
#define RESOLUTION_LOC			1     /* 分辨率参数在配置数组中的起始偏移 */
#define MAX_CONTACTS_LOC		5     /* 最大触摸点数在配置数组中的偏移 */
#define TRIGGER_LOC			6     /* 中断触发方式在配置数组中的偏移 */

/**
 * 轮询模式数据读取间隔
 * 当驱动工作在轮询模式(而非中断模式)时, 每隔此时间查询一次芯片的触摸状态
 * 17ms间隔对应约58.8Hz的采样率, 与绝大多数消费电子设备的60fps屏幕刷新率对齐
 * 既能保证触摸响应的流畅度, 又能避免过高的CPU占用率
 * 
 * 注: 正常工作时驱动优先使用中断模式, 轮询模式仅用于:
 * 1. 中断引脚硬件故障或未正确连接时的降级方案
 * 2. 驱动调试阶段排查中断问题
 * 3. 部分不支持硬件中断的特殊平台
 * 频率（单位：赫兹 Hz，即 "次 / 秒"）与周期（单位：秒 s，即 "每次耗时"）互为倒数：
 * 频率 f = 1 / 周期 T
 * f = 1 / 0.017 ≈ 58.8235 Hz
 * 
 */
#define GOODIX_POLL_INTERVAL_MS		17	/* 17ms ≈ 60fps 屏幕刷新率 */

/**
 * ACPI GPIO支持开关
 * 
 * 背景说明:
 * Goodix触控驱动在不同架构平台上的GPIO资源获取方式差异很大:
 * - ARM/ARM64平台: 统一通过设备树(Device Tree)描述和获取GPIO引脚
 * - x86平台: 绝大多数设备通过ACPI(高级配置与电源接口)表描述硬件资源
 * 
 * x86平台上的ACPI GPIO实现存在一些特殊性:
 * 1. GPIO编号可能不是全局唯一的, 需要通过ACPI句柄解析
 * 2. 部分厂商的ACPI表对Goodix触控屏的GPIO描述不规范
 * 3. 需要特殊处理GPIO的中断触发方式和电源管理
 * 
 * 此条件编译仅在同时满足以下两个条件时启用ACPI专用GPIO处理代码:
 * 1. 编译目标为x86架构(CONFIG_X86=y)
 * 2. 内核启用了ACPI支持(CONFIG_ACPI=y)
 * 
 * 启用后驱动会使用acpi_get_gpio()等ACPI专用API来获取复位和中断引脚
 */
#if defined CONFIG_X86 && defined CONFIG_ACPI
#define ACPI_GPIO_SUPPORT
#endif

struct goodix_chip_id {
	const char *id;
	const struct goodix_chip_data *data;
};

/**
 * goodix_check_cfg_8 - 校验8位校验和格式的触控芯片配置数据
 * @ts: Goodix触控驱动私有数据结构指针
 * @cfg: 从芯片读取的配置数据缓冲区首地址
 * @len: 配置数据的总长度(字节)
 *
 * 用于GT911/GT9111等早期型号芯片的配置数据校验
 * 采用8位累加和校验算法: 所有配置字节相加后取低8位,
 * 与配置数据最后一个字节的校验和值比较
 *
 * 返回: 0 校验成功, 配置数据有效
 *       -EINVAL 校验失败, 配置数据损坏或不完整
 */
static int goodix_check_cfg_8(struct goodix_ts_data *ts,
			      const u8 *cfg, int len);

/**
 * goodix_check_cfg_16 - 校验16位校验和格式的触控芯片配置数据
 * @ts: Goodix触控驱动私有数据结构指针
 * @cfg: 从芯片读取的配置数据缓冲区首地址
 * @len: 配置数据的总长度(字节)
 *
 * 用于GT9271/GT928/GT967等新型号芯片的配置数据校验
 * 采用16位累加和校验算法: 所有配置字节按16位字相加,
 * 与配置数据最后两个字节的校验和值比较
 *
 * 返回: 0 校验成功, 配置数据有效
 *       -EINVAL 校验失败, 配置数据损坏或不完整
 */
static int goodix_check_cfg_16(struct goodix_ts_data *ts,
			       const u8 *cfg, int len);
static void goodix_calc_cfg_checksum_8(struct goodix_ts_data *ts);
static void goodix_calc_cfg_checksum_16(struct goodix_ts_data *ts);

static const struct goodix_chip_data gt1x_chip_data = {
	.config_addr		= GOODIX_GT1X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_GT9X_LENGTH,
	.check_config		= goodix_check_cfg_16,
	.calc_config_checksum	= goodix_calc_cfg_checksum_16,
};

static const struct goodix_chip_data gt911_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_911_LENGTH,
	.check_config		= goodix_check_cfg_8,
	.calc_config_checksum	= goodix_calc_cfg_checksum_8,
};

static const struct goodix_chip_data gt967_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_967_LENGTH,
	.check_config		= goodix_check_cfg_8,
	.calc_config_checksum	= goodix_calc_cfg_checksum_8,
};

static const struct goodix_chip_data gt9x_chip_data = {
	.config_addr		= GOODIX_GT9X_REG_CONFIG_DATA,
	.config_len		= GOODIX_CONFIG_GT9X_LENGTH,
	.check_config		= goodix_check_cfg_8,
	.calc_config_checksum	= goodix_calc_cfg_checksum_8,
};

static const struct goodix_chip_id goodix_chip_ids[] = {
	{ .id = "1151", .data = &gt1x_chip_data },
	{ .id = "1158", .data = &gt1x_chip_data },
	{ .id = "5663", .data = &gt1x_chip_data },
	{ .id = "5688", .data = &gt1x_chip_data },
	{ .id = "917S", .data = &gt1x_chip_data },
	{ .id = "9286", .data = &gt1x_chip_data },

	{ .id = "911", .data = &gt911_chip_data },
	{ .id = "9271", .data = &gt911_chip_data },
	{ .id = "9110", .data = &gt911_chip_data },
	{ .id = "9111", .data = &gt911_chip_data },
	{ .id = "927", .data = &gt911_chip_data },
	{ .id = "928", .data = &gt911_chip_data },

	{ .id = "912", .data = &gt967_chip_data },
	{ .id = "9147", .data = &gt967_chip_data },
	{ .id = "967", .data = &gt967_chip_data },
	{ }
};

/**
 * Goodix触控芯片支持的中断触发类型列表
 * 
 * 汇顶GT9xx全系列触控芯片均支持4种硬件中断触发方式
 * 驱动会根据设备树(DT)或ACPI表中配置的"interrupts"属性,
 * 从此数组中选择对应的中断标志来调用request_irq()申请中断
 * 
 * 数组索引与驱动内部中断类型编号的对应关系:
 * [0] = 上升沿触发 (对应GOODIX_INT_TRIGGER=1)
 * [1] = 下降沿触发 (对应GOODIX_INT_TRIGGER=0)
 * [2] = 低电平触发
 * [3] = 高电平触发
 * 
 * 注: 绝大多数消费电子设备使用上升沿或下降沿触发方式
 * 电平触发方式仅在部分工业级设备或特殊硬件设计中使用
 */
static const unsigned long goodix_irq_flags[] = {
	IRQ_TYPE_EDGE_RISING,    /* 中断触发类型: 上升沿触发 */
	IRQ_TYPE_EDGE_FALLING,   /* 中断触发类型: 下降沿触发 */
	IRQ_TYPE_LEVEL_LOW,      /* 中断触发类型: 低电平触发 */
	IRQ_TYPE_LEVEL_HIGH,     /* 中断触发类型: 高电平触发 */
};

static const struct dmi_system_id nine_bytes_report[] = {
#if defined(CONFIG_DMI) && defined(CONFIG_X86)
	{
		/* Lenovo Yoga Book X90F / X90L */
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "CHERRYVIEW D1 PLATFORM"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "YETI-11"),
		}
	},
	{
		/* Lenovo Yoga Book X91F / X91L */
		.matches = {
			/* Non exact match to match F + L versions */
			DMI_MATCH(DMI_PRODUCT_NAME, "Lenovo YB1-X91"),
		}
	},
#endif
	{}
};

/*
 * Those tablets have their x coordinate inverted
 */
static const struct dmi_system_id inverted_x_screen[] = {
#if defined(CONFIG_DMI) && defined(CONFIG_X86)
	{
		.ident = "Cube I15-TC",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Cube"),
			DMI_MATCH(DMI_PRODUCT_NAME, "I15-TC")
		},
	},
#endif
	{}
};

/**
 * goodix_i2c_read - read data from a register of the i2c slave device.
 *从i2c从设备的寄存器中读取数据
 * @client: i2c device.
 * @reg: the register to read from.
 * @buf: raw write data buffer.
 * @len: length of the buffer to write
 */
int goodix_i2c_read(struct i2c_client *client, u16 reg, u8 *buf, int len)
{
struct i2c_msg msgs[2];          /* I2C消息数组，读操作需要2个消息 */
	__be16 wbuf = cpu_to_be16(reg);  /* 将主机字节序的寄存器地址转换为大端字节序 */
	int ret;

	/* 第1个消息: 写操作，发送要读取的寄存器地址 */
	msgs[0].flags = 0;               /* 0表示写操作(I2C_M_WR) */
	msgs[0].addr  = client->addr;    /* I2C从设备地址 */
	msgs[0].len   = 2;               /* 发送2字节的16位寄存器地址 */
	msgs[0].buf   = (u8 *)&wbuf;     /* 指向转换后的大端地址缓冲区 */

	/* 第2个消息: 读操作，从指定寄存器读取数据 */
	msgs[1].flags = I2C_M_RD;        /* I2C_M_RD表示读操作 */
	msgs[1].addr  = client->addr;    /* 同一个从设备地址 */
	msgs[1].len   = len;             /* 要读取的数据长度 */
	msgs[1].buf   = buf;             /* 指向接收数据的缓冲区 */

	/* 执行I2C传输，发送2个消息 */
	ret = i2c_transfer(client->adapter, msgs, 2);
	
	/* 处理返回值: i2c_transfer返回成功传输的消息数 */
	if (ret >= 0)
		/* 只有当2个消息都成功传输时才返回0，否则返回I/O错误 */
		ret = (ret == ARRAY_SIZE(msgs) ? 0 : -EIO);

	/* 如果发生错误，打印调试信息 */
	if (ret)
		dev_err(&client->dev, "Error reading %d bytes from 0x%04x: %d\n",
			len, reg, ret);
	return ret;
}

/**
 * goodix_i2c_write - write data to a register of the i2c slave device.
 *
 * @client: i2c device.
 * @reg: the register to write to.
 * @buf: raw data buffer to write.
 * @len: length of the buffer to write
 */
int goodix_i2c_write(struct i2c_client *client, u16 reg, const u8 *buf, int len)
{
	u8 *addr_buf;                   /* 用于存储"地址+数据"的临时缓冲区 */
	struct i2c_msg msg;             /* I2C写消息，写操作只需要1个消息 */
	int ret;

	/* 分配临时缓冲区: 2字节寄存器地址 + len字节数据 */
	addr_buf = kmalloc(len + 2, GFP_KERNEL);
	if (!addr_buf)
		return -ENOMEM;

	/* 填充16位寄存器地址(高字节在前，符合Goodix芯片的大端要求) */
	addr_buf[0] = reg >> 8;         /* 寄存器地址高8位 */
	addr_buf[1] = reg & 0xFF;       /* 寄存器地址低8位 */
	
	/* 将要写入的数据复制到临时缓冲区的地址之后 */
	memcpy(&addr_buf[2], buf, len);

	/* 构造I2C写消息 */
	msg.flags = 0;                  /* 0表示写操作 */
	msg.addr = client->addr;        /* I2C从设备地址 */
	msg.buf = addr_buf;             /* 指向包含地址和数据的缓冲区 */
	msg.len = len + 2;              /* 总传输长度: 地址(2字节) + 数据(len字节) */

	/* 执行I2C传输，发送1个消息 */
	ret = i2c_transfer(client->adapter, &msg, 1);
	
	/* 处理返回值 */
	if (ret >= 0)
		ret = (ret == 1 ? 0 : -EIO);

	/* 释放临时缓冲区，无论传输成功与否都必须释放 */
	kfree(addr_buf);

	/* 如果发生错误，打印调试信息 */
	if (ret)
		dev_err(&client->dev, "Error writing %d bytes to 0x%04x: %d\n",
			len, reg, ret);
	return ret;
}

int goodix_i2c_write_u8(struct i2c_client *client, u16 reg, u8 value)
{
	return goodix_i2c_write(client, reg, &value, sizeof(value));
}

static const struct goodix_chip_data *goodix_get_chip_data(const char *id)
{
	unsigned int i;

	for (i = 0; goodix_chip_ids[i].id; i++) {
		if (!strcmp(goodix_chip_ids[i].id, id))
			return goodix_chip_ids[i].data;
	}

	return &gt9x_chip_data;
}

static int goodix_ts_read_input_report(struct goodix_ts_data *ts, u8 *data)
{
	unsigned long max_timeout;
	int touch_num;
	int error;
	u16 addr = GOODIX_READ_COOR_ADDR;
	/*
	 * We are going to read 1-byte header,
	 * ts->contact_size * max(1, touch_num) bytes of coordinates
	 * and 1-byte footer which contains the touch-key code.
	 */
	const int header_contact_keycode_size = 1 + ts->contact_size + 1;

	/*
	 * The 'buffer status' bit, which indicates that the data is valid, is
	 * not set as soon as the interrupt is raised, but slightly after.
	 * This takes around 10 ms to happen, so we poll for 20 ms.
	 */
	max_timeout = jiffies + msecs_to_jiffies(GOODIX_BUFFER_STATUS_TIMEOUT);
	do {
		error = goodix_i2c_read(ts->client, addr, data,
					header_contact_keycode_size);
		if (error)
			return error;

		if (data[0] & GOODIX_BUFFER_STATUS_READY) {
			touch_num = data[0] & 0x0f;
			if (touch_num > ts->max_touch_num)
				return -EPROTO;

			if (touch_num > 1) {
				addr += header_contact_keycode_size;
				data += header_contact_keycode_size;
				error = goodix_i2c_read(ts->client,
						addr, data,
						ts->contact_size *
							(touch_num - 1));
				if (error)
					return error;
			}

			return touch_num;
		}

		if (data[0] == 0 && ts->firmware_name) {
			if (goodix_handle_fw_request(ts))
				return 0;
		}

		if (!ts->client->irq)
			/*
			 * No point in retrying if polling, particularly as some
			 * versions continuously report "not ready" if there are
			 * no touch points.
			 */
			break;

		usleep_range(1000, 2000); /* Poll every 1 - 2 ms */
	} while (time_before(jiffies, max_timeout));

	/*
	 * The Goodix panel will send spurious interrupts after a
	 * 'finger up' event, which will always cause a timeout.
	 */
	return -ENOMSG;
}

static int goodix_create_pen_input(struct goodix_ts_data *ts)
{
	struct device *dev = &ts->client->dev;
	struct input_dev *input;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input_copy_abs(input, ABS_X, ts->input_dev, ABS_MT_POSITION_X);
	input_copy_abs(input, ABS_Y, ts->input_dev, ABS_MT_POSITION_Y);
	/*
	 * The resolution of these touchscreens is about 10 units/mm, the actual
	 * resolution does not matter much since we set INPUT_PROP_DIRECT.
	 * Userspace wants something here though, so just set it to 10 units/mm.
	 */
	input_abs_set_res(input, ABS_X, 10);
	input_abs_set_res(input, ABS_Y, 10);
	input_set_abs_params(input, ABS_PRESSURE, 0, 255, 0, 0);

	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_capability(input, EV_KEY, BTN_TOOL_PEN);
	input_set_capability(input, EV_KEY, BTN_STYLUS);
	input_set_capability(input, EV_KEY, BTN_STYLUS2);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);

	input->name = "Goodix Active Pen";
	input->phys = "input/pen";
	input->id.bustype = BUS_I2C;
	input->id.vendor = 0x0416;
	if (kstrtou16(ts->id, 10, &input->id.product))
		input->id.product = 0x1001;
	input->id.version = ts->version;

	ts->input_pen = input;
	return 0;
}

static void goodix_ts_report_pen_down(struct goodix_ts_data *ts, u8 *data)
{
	int input_x, input_y, input_w, error;
	u8 key_value;

	if (!ts->pen_input_registered) {
		error = input_register_device(ts->input_pen);
		ts->pen_input_registered = (error == 0) ? 1 : error;
	}

	if (ts->pen_input_registered < 0)
		return;

	if (ts->contact_size == 9) {
		input_x = get_unaligned_le16(&data[4]);
		input_y = get_unaligned_le16(&data[6]);
		input_w = get_unaligned_le16(&data[8]);
	} else {
		input_x = get_unaligned_le16(&data[2]);
		input_y = get_unaligned_le16(&data[4]);
		input_w = get_unaligned_le16(&data[6]);
	}

	touchscreen_report_pos(ts->input_pen, &ts->prop, input_x, input_y, false);
	input_report_abs(ts->input_pen, ABS_PRESSURE, input_w);

	input_report_key(ts->input_pen, BTN_TOUCH, 1);
	input_report_key(ts->input_pen, BTN_TOOL_PEN, 1);

	if (data[0] & GOODIX_HAVE_KEY) {
		key_value = data[1 + ts->contact_size];
		input_report_key(ts->input_pen, BTN_STYLUS, key_value & 0x10);
		input_report_key(ts->input_pen, BTN_STYLUS2, key_value & 0x20);
	} else {
		input_report_key(ts->input_pen, BTN_STYLUS, 0);
		input_report_key(ts->input_pen, BTN_STYLUS2, 0);
	}

	input_sync(ts->input_pen);
}

static void goodix_ts_report_pen_up(struct goodix_ts_data *ts)
{
	if (!ts->input_pen)
		return;

	input_report_key(ts->input_pen, BTN_TOUCH, 0);
	input_report_key(ts->input_pen, BTN_TOOL_PEN, 0);
	input_report_key(ts->input_pen, BTN_STYLUS, 0);
	input_report_key(ts->input_pen, BTN_STYLUS2, 0);

	input_sync(ts->input_pen);
}

static void goodix_ts_report_touch_8b(struct goodix_ts_data *ts, u8 *coor_data)
{
	int id = coor_data[0] & 0x0F;
	int input_x = get_unaligned_le16(&coor_data[1]);
	int input_y = get_unaligned_le16(&coor_data[3]);
	int input_w = get_unaligned_le16(&coor_data[5]);

	input_mt_slot(ts->input_dev, id);
	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
	touchscreen_report_pos(ts->input_dev, &ts->prop,
			       input_x, input_y, true);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, input_w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, input_w);
}

static void goodix_ts_report_touch_9b(struct goodix_ts_data *ts, u8 *coor_data)
{
	int id = coor_data[1] & 0x0F;
	int input_x = get_unaligned_le16(&coor_data[3]);
	int input_y = get_unaligned_le16(&coor_data[5]);
	int input_w = get_unaligned_le16(&coor_data[7]);

	input_mt_slot(ts->input_dev, id);
	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
	touchscreen_report_pos(ts->input_dev, &ts->prop,
			       input_x, input_y, true);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, input_w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, input_w);
}

static void goodix_ts_release_keys(struct goodix_ts_data *ts)
{
	int i;

	for (i = 0; i < GOODIX_MAX_KEYS; i++)
		input_report_key(ts->input_dev, ts->keymap[i], 0);
}

static void goodix_ts_report_key(struct goodix_ts_data *ts, u8 *data)
{
	int touch_num;
	u8 key_value;
	int i;

	if (data[0] & GOODIX_HAVE_KEY) {
		touch_num = data[0] & 0x0f;
		key_value = data[1 + ts->contact_size * touch_num];
		for (i = 0; i < GOODIX_MAX_KEYS; i++)
			if (key_value & BIT(i))
				input_report_key(ts->input_dev,
						 ts->keymap[i], 1);
	} else {
		goodix_ts_release_keys(ts);
	}
}

/**
 * goodix_process_events - Process incoming events
 *
 * @ts: our goodix_ts_data pointer
 *
 * Called when the IRQ is triggered. Read the current device state, and push
 * the input events to the user space.
 */
static void goodix_process_events(struct goodix_ts_data *ts)
{
	u8  point_data[2 + GOODIX_MAX_CONTACT_SIZE * GOODIX_MAX_CONTACTS];
	int touch_num;
	int i;

	touch_num = goodix_ts_read_input_report(ts, point_data);
	if (touch_num < 0)
		return;

	/* The pen being down is always reported as a single touch */
	if (touch_num == 1 && (point_data[1] & 0x80)) {
		goodix_ts_report_pen_down(ts, point_data);
		goodix_ts_release_keys(ts);
		goto sync; /* Release any previously registered touches */
	} else {
		goodix_ts_report_pen_up(ts);
	}

	goodix_ts_report_key(ts, point_data);

	for (i = 0; i < touch_num; i++)
		if (ts->contact_size == 9)
			goodix_ts_report_touch_9b(ts,
				&point_data[1 + ts->contact_size * i]);
		else
			goodix_ts_report_touch_8b(ts,
				&point_data[1 + ts->contact_size * i]);

sync:
	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);
}

static void goodix_ts_work_i2c_poll(struct input_dev *input)
{
	struct goodix_ts_data *ts = input_get_drvdata(input);

	goodix_process_events(ts);
	goodix_i2c_write_u8(ts->client, GOODIX_READ_COOR_ADDR, 0);
}

/**
 * goodix_ts_irq_handler - The IRQ handler
 *
 * @irq: interrupt number.
 * @dev_id: private data pointer.
 */
static irqreturn_t goodix_ts_irq_handler(int irq, void *dev_id)
{
	struct goodix_ts_data *ts = dev_id;

	goodix_process_events(ts);
	goodix_i2c_write_u8(ts->client, GOODIX_READ_COOR_ADDR, 0);

	return IRQ_HANDLED;
}

static void goodix_enable_irq(struct goodix_ts_data *ts)
{
	if (ts->client->irq)
		enable_irq(ts->client->irq);
}

static void goodix_disable_irq(struct goodix_ts_data *ts)
{
	if (ts->client->irq)
		disable_irq(ts->client->irq);
}

static void goodix_free_irq(struct goodix_ts_data *ts)
{
	if (ts->client->irq)
		devm_free_irq(&ts->client->dev, ts->client->irq, ts);
}

static int goodix_request_irq(struct goodix_ts_data *ts)
{
	if (!ts->client->irq)
		return 0;

	return devm_request_threaded_irq(&ts->client->dev, ts->client->irq,
					 NULL, goodix_ts_irq_handler,
					 ts->irq_flags, ts->client->name, ts);
}

static int goodix_check_cfg_8(struct goodix_ts_data *ts, const u8 *cfg, int len)
{
	int i, raw_cfg_len = len - 2;
	u8 check_sum = 0;

	for (i = 0; i < raw_cfg_len; i++)
		check_sum += cfg[i];
	check_sum = (~check_sum) + 1;
	if (check_sum != cfg[raw_cfg_len]) {
		dev_err(&ts->client->dev,
			"The checksum of the config fw is not correct");
		return -EINVAL;
	}

	if (cfg[raw_cfg_len + 1] != 1) {
		dev_err(&ts->client->dev,
			"Config fw must have Config_Fresh register set");
		return -EINVAL;
	}

	return 0;
}

static void goodix_calc_cfg_checksum_8(struct goodix_ts_data *ts)
{
	int i, raw_cfg_len = ts->chip->config_len - 2;
	u8 check_sum = 0;

	for (i = 0; i < raw_cfg_len; i++)
		check_sum += ts->config[i];
	check_sum = (~check_sum) + 1;

	ts->config[raw_cfg_len] = check_sum;
	ts->config[raw_cfg_len + 1] = 1; /* Set "config_fresh" bit */
}

static int goodix_check_cfg_16(struct goodix_ts_data *ts, const u8 *cfg,
			       int len)
{
	int i, raw_cfg_len = len - 3;
	u16 check_sum = 0;

	for (i = 0; i < raw_cfg_len; i += 2)
		check_sum += get_unaligned_be16(&cfg[i]);
	check_sum = (~check_sum) + 1;
	if (check_sum != get_unaligned_be16(&cfg[raw_cfg_len])) {
		dev_err(&ts->client->dev,
			"The checksum of the config fw is not correct");
		return -EINVAL;
	}

	if (cfg[raw_cfg_len + 2] != 1) {
		dev_err(&ts->client->dev,
			"Config fw must have Config_Fresh register set");
		return -EINVAL;
	}

	return 0;
}

static void goodix_calc_cfg_checksum_16(struct goodix_ts_data *ts)
{
	int i, raw_cfg_len = ts->chip->config_len - 3;
	u16 check_sum = 0;

	for (i = 0; i < raw_cfg_len; i += 2)
		check_sum += get_unaligned_be16(&ts->config[i]);
	check_sum = (~check_sum) + 1;

	put_unaligned_be16(check_sum, &ts->config[raw_cfg_len]);
	ts->config[raw_cfg_len + 2] = 1; /* Set "config_fresh" bit */
}

/**
 * goodix_check_cfg - Checks if config fw is valid
 *
 * @ts: goodix_ts_data pointer
 * @cfg: firmware config data
 * @len: config data length
 */
static int goodix_check_cfg(struct goodix_ts_data *ts, const u8 *cfg, int len)
{
	if (len < GOODIX_CONFIG_MIN_LENGTH ||
	    len > GOODIX_CONFIG_MAX_LENGTH) {
		dev_err(&ts->client->dev,
			"The length of the config fw is not correct");
		return -EINVAL;
	}

	return ts->chip->check_config(ts, cfg, len);
}

/**
 * goodix_send_cfg - Write fw config to device
 *
 * @ts: goodix_ts_data pointer
 * @cfg: config firmware to write to device
 * @len: config data length
 */
int goodix_send_cfg(struct goodix_ts_data *ts, const u8 *cfg, int len)
{
	int error;

	error = goodix_check_cfg(ts, cfg, len);
	if (error)
		return error;

	error = goodix_i2c_write(ts->client, ts->chip->config_addr, cfg, len);
	if (error)
		return error;

	dev_dbg(&ts->client->dev, "Config sent successfully.");

	/* Let the firmware reconfigure itself, so sleep for 10ms */
	usleep_range(10000, 11000);

	return 0;
}

#ifdef ACPI_GPIO_SUPPORT
static int goodix_pin_acpi_direction_input(struct goodix_ts_data *ts)
{
	acpi_handle handle = ACPI_HANDLE(&ts->client->dev);
	acpi_status status;

	status = acpi_evaluate_object(handle, "INTI", NULL, NULL);
	return ACPI_SUCCESS(status) ? 0 : -EIO;
}

static int goodix_pin_acpi_output_method(struct goodix_ts_data *ts, int value)
{
	acpi_handle handle = ACPI_HANDLE(&ts->client->dev);
	acpi_status status;

	status = acpi_execute_simple_method(handle, "INTO", value);
	return ACPI_SUCCESS(status) ? 0 : -EIO;
}
#else
/*
 * 函数：goodix_pin_acpi_direction_input
 * 描述：ACPI 平台专用的中断引脚设为输入的方法
 * 备注：仅在支持 ACPI 控制的 x86 平台生效；当前设备无 ACPI 支持时，直接报错返回
 * 参数：ts - 触控设备私有数据结构体指针
 * 返回：错误码 -EINVAL 表示不支持该操作
 */
static int goodix_pin_acpi_direction_input(struct goodix_ts_data *ts)
{
    // 打印错误日志：在无ACPI支持的设备上调用了该函数
	dev_err(&ts->client->dev,
		"%s called on device without ACPI support\n", __func__);
    // 返回无效参数错误码
	return -EINVAL;
}

/*
 * 函数：goodix_pin_acpi_output_method
 * 描述：ACPI 平台专用的中断引脚输出电平控制方法
 * 备注：通过 ACPI 控制方法设置引脚电平，无ACPI支持时调用直接报错
 * 参数：ts - 触控设备私有数据；value - 要输出的逻辑电平值
 * 返回：错误码 -EINVAL 表示不支持该操作
 */
static int goodix_pin_acpi_output_method(struct goodix_ts_data *ts, int value)
{
    // 打印错误日志：在无ACPI支持的设备上调用了该函数
	dev_err(&ts->client->dev,
		"%s called on device without ACPI support\n", __func__);
    // 返回无效参数错误码
	return -EINVAL;
}
#endif  // 对应上方的 ACPI 条件编译宏，非ACPI平台编译时会排除以上两个函数

/*
 * 函数：goodix_irq_direction_output
 * 描述：将中断(INT)引脚配置为输出模式，并输出指定电平
 * 核心逻辑：根据当前引脚的访问方式（GPIO/ACPI等），选择对应的底层API设置引脚
 * 参数：ts - 触控设备私有数据；value - 要输出的物理电平值（0/1）
 * 返回：0 成功，负值 失败错误码
 */
static int goodix_irq_direction_output(struct goodix_ts_data *ts, int value)
{
    // 根据引脚访问方式分支处理
	switch (ts->irq_pin_access_method) {
    // 情况1：未配置任何引脚访问方式
	case IRQ_PIN_ACCESS_NONE:
        // 打印错误：调用函数前未设置中断引脚的访问方式
		dev_err(&ts->client->dev,
			"%s called without an irq_pin_access_method set\n",
			__func__);
		return -EINVAL;

    // 情况2：普通GPIO模式（设备树平台默认使用该分支，树莓派等ARM设备走这里）
	case IRQ_PIN_ACCESS_GPIO:
        // Goodix 复位/地址选通要求驱动 INT 引脚的物理电平，不能做逻辑反相
		return gpiod_direction_output_raw(ts->gpiod_int, value);

    // 情况3：ACPI平台的GPIO模式
	case IRQ_PIN_ACCESS_ACPI_GPIO:
		/*
		 * 注释说明：
		 * Goodix中断默认是下降沿触发，在ACPI描述中会被标记为低电平有效
		 * 如果用普通gpiod_direction_output，会自动做极性反转，导致输出电平与预期相反
		 * 因此使用 _raw 后缀函数，直接写入物理电平，绕过极性转换逻辑
		 */
		return gpiod_direction_output_raw(ts->gpiod_int, value);

    // 情况4：ACPI方法控制模式（通过ACPI固件方法操作引脚）
	case IRQ_PIN_ACCESS_ACPI_METHOD:
        // 调用ACPI专用的输出控制函数
		return goodix_pin_acpi_output_method(ts, value);
	}

	return -EINVAL; /* 理论上永远不会执行到这里，兜底返回错误 */
}

/*
 * 函数：goodix_irq_direction_input
 * 描述：将中断(INT)引脚配置为输入模式，用于接收芯片发来的触摸中断信号
 * 参数：ts - 触控设备私有数据
 * 返回：0 成功，负值 失败错误码
 */
static int goodix_irq_direction_input(struct goodix_ts_data *ts)
{
    // 根据引脚访问方式分支处理
	switch (ts->irq_pin_access_method) {
    // 情况1：未配置任何引脚访问方式
	case IRQ_PIN_ACCESS_NONE:
        // 打印错误：调用前未设置引脚访问方式
		dev_err(&ts->client->dev,
			"%s called without an irq_pin_access_method set\n",
			__func__);
		return -EINVAL;

    // 情况2：普通GPIO模式（设备树平台使用）
	case IRQ_PIN_ACCESS_GPIO:
        // 标准gpiod接口：设置为输入模式
		return gpiod_direction_input(ts->gpiod_int);

    // 情况3：ACPI平台的GPIO模式
	case IRQ_PIN_ACCESS_ACPI_GPIO:
        // 设置为输入模式，输入模式无需关心极性转换
		return gpiod_direction_input(ts->gpiod_int);

    // 情况4：ACPI方法控制模式
	case IRQ_PIN_ACCESS_ACPI_METHOD:
        // 调用ACPI专用的输入设置函数
		return goodix_pin_acpi_direction_input(ts);
	}

	return -EINVAL; /* 兜底返回错误，正常流程不会走到这里 */
}

/*
 * 函数：goodix_int_sync
 * 描述：Goodix芯片中断同步函数，是上电初始化的核心步骤
 * 功能说明：
 *   1. Goodix芯片会在上电复位时，采样INT引脚的电平来确定自身的I2C地址（0x14 / 0x5d）
 *   2. 该函数主动控制INT引脚输出指定电平并保持时序，确保芯片I2C地址固定、与驱动预期一致
 *   3. 完成后将INT引脚切回输入模式，准备接收触摸中断
 * 参数：ts - 触控设备私有数据
 * 返回：0 同步成功，负值 失败
 */
int goodix_int_sync(struct goodix_ts_data *ts)
{
	int error;

	goodix_trace(&ts->client->dev, "int_sync: start\n");

    // 第一步：将中断引脚配置为输出模式，并输出低电平
    // 目的：拉低INT引脚，让芯片在上电时采样到低电平，确定I2C地址为0x5d
	goodix_trace(&ts->client->dev, "int_sync: step1 set INT output low\n");
	error = goodix_irq_direction_output(ts, 0);
	if (error)
		goto error;  // 配置失败则跳转到错误处理

    // 延时50毫秒，对应芯片数据手册中的 T5 时序要求
    // 必须保持足够时长，确保芯片正确采样引脚电平、完成内部初始化
	goodix_trace(&ts->client->dev, "int_sync: step2 hold INT low for 50ms\n");
	msleep(50);				/* T5: 50ms */

    // 第二步：将中断引脚重新配置为输入模式
    // 目的：释放引脚控制权，让芯片可以通过该引脚向主控发送触摸中断信号
	goodix_trace(&ts->client->dev, "int_sync: step3 switch INT back to input\n");
	error = goodix_irq_direction_input(ts);
	if (error)
		goto error;  // 配置失败则跳转到错误处理

	goodix_trace(&ts->client->dev, "int_sync: done\n");
	return 0;  // 同步成功返回0

error:
    // 任意步骤失败，打印错误日志
	dev_err(&ts->client->dev, "Controller irq sync failed.\n");
	return error;  // 返回对应错误码
}

/**
 * goodix_reset_no_int_sync - 复位触摸芯片，并将中断线置于输出模式（用于 I2C 地址选择）
 * @ts: Goodix 触摸数据结构指针
 *
 * 该函数按照 Goodix 芯片手册的复位时序执行硬件复位。
 * 复位过程中使用中断引脚输出高低电平来选择 I2C 从设备地址，
 * 完成复位后将复位引脚（RST）设置为输入高阻态以节省功耗（非 ACPI 平台）。
 *
 * 返回: 0 表示成功，负值表示失败。
 */
int goodix_reset_no_int_sync(struct goodix_ts_data *ts)
{
	int error;

	goodix_trace(&ts->client->dev, "reset_no_int_sync: start addr=0x%02x\n",
		     ts->client->addr);

	/*
	 * 步骤 1: 将复位引脚设为输出，并输出低电平，开始复位。
	 * 这里使用 gpiod_direction_output_raw() 直接输出物理低电平，
	 * 不受设备树中 GPIO_ACTIVE_LOW/ HIGH 极性标志影响。
	 */
	goodix_trace(&ts->client->dev, "reset_no_int_sync: step1 set RST low\n");
	error = gpiod_direction_output_raw(ts->gpiod_rst, 0);
	if (error)
		goto error;

	goodix_trace(&ts->client->dev, "reset_no_int_sync: step2 hold RST low for 20ms\n");
	msleep(20);				/* T2: 复位低电平保持时间，手册要求 > 10ms，这里延长至 20ms */

	/*
	 * 步骤 2: 通过中断引脚输出电平来选择 I2C 从设备地址。
	 * goodix_irq_direction_output 会将中断引脚配置为输出，
	 * 并输出高或低电平，根据传入的参数决定：
	 *   - 如果 ts->client->addr == 0x14 (7位地址 0x14)，则输出高电平 (HIGH: 0x28/0x29)
	 *   - 否则输出低电平 (LOW: 0xBA/0xBB)
	 * 芯片会在复位释放前采样该电平，以确定 I2C 地址的高 7 位是 0x28/0x29 还是 0xBA/0xBB。
	 */
	goodix_trace(&ts->client->dev,
		     "reset_no_int_sync: step3 drive INT to %u for address select\n",
		     ts->client->addr == 0x14);
	error = goodix_irq_direction_output(ts, ts->client->addr == 0x14);
	if (error)
		goto error;

	goodix_trace(&ts->client->dev, "reset_no_int_sync: step4 wait INT setup 100us-2ms\n");
	usleep_range(100, 2000);		/* T3: 中断引脚建立时间，手册要求 > 100us */

	/*
	 * 步骤 3: 将复位引脚拉高，结束复位。
	 * 芯片在复位释放时锁存中断引脚上的电平，从而确定 I2C 地址。
	 */
	goodix_trace(&ts->client->dev, "reset_no_int_sync: step5 release RST high\n");
	error = gpiod_direction_output_raw(ts->gpiod_rst, 1);
	if (error)
		goto error;

	goodix_trace(&ts->client->dev, "reset_no_int_sync: step6 wait 6-10ms after reset release\n");
	usleep_range(6000, 10000);		/* T4: 复位恢复后等待时间，手册要求 > 5ms，这里 6-10ms */

	/*
	 * 步骤 4 (可选): 将复位引脚重新置为输入（高阻态），以节省功耗。
	 * 仅在 GPIO 方式访问引脚时执行（非 ACPI 情况），因为某些 ACPI 平台
	 * 没有外部上拉，复位引脚必须保持高电平输出才能正常工作。
	 */
	if (ts->irq_pin_access_method == IRQ_PIN_ACCESS_GPIO) {
		goodix_trace(&ts->client->dev, "reset_no_int_sync: step7 switch RST to input\n");
		error = gpiod_direction_input(ts->gpiod_rst);
		if (error)
			goto error;
	}

	goodix_trace(&ts->client->dev, "reset_no_int_sync: done\n");
	return 0;

error:
	dev_err(&ts->client->dev, "Controller reset failed.\n");
	return error;
}

/**
 * goodix_reset - Reset device during power on
 *
 * @ts: goodix_ts_data pointer
 */
static int goodix_reset(struct goodix_ts_data *ts)
{
	int error;

	goodix_trace(&ts->client->dev, "reset: start\n");
	error = goodix_reset_no_int_sync(ts);
	if (error)
		return error;

	goodix_trace(&ts->client->dev, "reset: run INT sync after reset\n");
	return goodix_int_sync(ts);
}

#ifdef ACPI_GPIO_SUPPORT
static const struct acpi_gpio_params first_gpio = { 0, 0, false };
static const struct acpi_gpio_params second_gpio = { 1, 0, false };

static const struct acpi_gpio_mapping acpi_goodix_int_first_gpios[] = {
	{ GOODIX_GPIO_INT_NAME "-gpios", &first_gpio, 1 },
	{ GOODIX_GPIO_RST_NAME "-gpios", &second_gpio, 1 },
	{ },
};

static const struct acpi_gpio_mapping acpi_goodix_int_last_gpios[] = {
	{ GOODIX_GPIO_RST_NAME "-gpios", &first_gpio, 1 },
	{ GOODIX_GPIO_INT_NAME "-gpios", &second_gpio, 1 },
	{ },
};

static const struct acpi_gpio_mapping acpi_goodix_reset_only_gpios[] = {
	{ GOODIX_GPIO_RST_NAME "-gpios", &first_gpio, 1 },
	{ },
};

static int goodix_resource(struct acpi_resource *ares, void *data)
{
	struct goodix_ts_data *ts = data;
	struct device *dev = &ts->client->dev;
	struct acpi_resource_gpio *gpio;

	if (acpi_gpio_get_irq_resource(ares, &gpio)) {
		if (ts->gpio_int_idx == -1) {
			ts->gpio_int_idx = ts->gpio_count;
		} else {
			dev_err(dev, "More then one GpioInt resource, ignoring ACPI GPIO resources\n");
			ts->gpio_int_idx = -2;
		}
		ts->gpio_count++;
	} else if (acpi_gpio_get_io_resource(ares, &gpio))
		ts->gpio_count++;

	return 0;
}

/*
 * This function gets called in case we fail to get the irq GPIO directly
 * because the ACPI tables lack GPIO-name to APCI _CRS index mappings
 * (no _DSD UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301 data).
 * In that case we add our own mapping and then goodix_get_gpio_config()
 * retries to get the GPIOs based on the added mapping.
 */
static int goodix_add_acpi_gpio_mappings(struct goodix_ts_data *ts)
{
	const struct acpi_gpio_mapping *gpio_mapping = NULL;
	struct device *dev = &ts->client->dev;
	LIST_HEAD(resources);
	int irq, ret;

	ts->gpio_count = 0;
	ts->gpio_int_idx = -1;
	ret = acpi_dev_get_resources(ACPI_COMPANION(dev), &resources,
				     goodix_resource, ts);
	if (ret < 0) {
		dev_err(dev, "Error getting ACPI resources: %d\n", ret);
		return ret;
	}

	acpi_dev_free_resource_list(&resources);

	/*
	 * CHT devices should have a GpioInt + a regular GPIO ACPI resource.
	 * Some CHT devices have a bug (where the also is bogus Interrupt
	 * resource copied from a previous BYT based generation). i2c-core-acpi
	 * will use the non-working Interrupt resource, fix this up.
	 */
	if (soc_intel_is_cht() && ts->gpio_count == 2 && ts->gpio_int_idx != -1) {
		irq = acpi_dev_gpio_irq_get(ACPI_COMPANION(dev), 0);
		if (irq > 0 && irq != ts->client->irq) {
			dev_warn(dev, "Overriding IRQ %d -> %d\n", ts->client->irq, irq);
			ts->client->irq = irq;
		}
	}

	/* Some devices with gpio_int_idx 0 list a third unused GPIO */
	if ((ts->gpio_count == 2 || ts->gpio_count == 3) && ts->gpio_int_idx == 0) {
		ts->irq_pin_access_method = IRQ_PIN_ACCESS_ACPI_GPIO;
		gpio_mapping = acpi_goodix_int_first_gpios;
	} else if (ts->gpio_count == 2 && ts->gpio_int_idx == 1) {
		ts->irq_pin_access_method = IRQ_PIN_ACCESS_ACPI_GPIO;
		gpio_mapping = acpi_goodix_int_last_gpios;
	} else if (ts->gpio_count == 1 && ts->gpio_int_idx == -1 &&
		   acpi_has_method(ACPI_HANDLE(dev), "INTI") &&
		   acpi_has_method(ACPI_HANDLE(dev), "INTO")) {
		dev_info(dev, "Using ACPI INTI and INTO methods for IRQ pin access\n");
		ts->irq_pin_access_method = IRQ_PIN_ACCESS_ACPI_METHOD;
		gpio_mapping = acpi_goodix_reset_only_gpios;
	} else if (soc_intel_is_byt() && ts->gpio_count == 2 && ts->gpio_int_idx == -1) {
		dev_info(dev, "No ACPI GpioInt resource, assuming that the GPIO order is reset, int\n");
		ts->irq_pin_access_method = IRQ_PIN_ACCESS_ACPI_GPIO;
		gpio_mapping = acpi_goodix_int_last_gpios;
	} else if (ts->gpio_count == 1 && ts->gpio_int_idx == 0) {
		/*
		 * On newer devices there is only 1 GpioInt resource and _PS0
		 * does the whole reset sequence for us.
		 */
		acpi_device_fix_up_power(ACPI_COMPANION(dev));

		/*
		 * Before the _PS0 call the int GPIO may have been in output
		 * mode and the call should have put the int GPIO in input mode,
		 * but the GPIO subsys cached state may still think it is
		 * in output mode, causing gpiochip_lock_as_irq() failure.
		 *
		 * Add a mapping for the int GPIO to make the
		 * gpiod_int = gpiod_get(..., GPIOD_IN) call succeed,
		 * which will explicitly set the direction to input.
		 */
		ts->irq_pin_access_method = IRQ_PIN_ACCESS_NONE;
		gpio_mapping = acpi_goodix_int_first_gpios;
	} else {
		dev_warn(dev, "Unexpected ACPI resources: gpio_count %d, gpio_int_idx %d\n",
			 ts->gpio_count, ts->gpio_int_idx);
		/*
		 * On some devices _PS0 does a reset for us and
		 * sometimes this is necessary for things to work.
		 */
		acpi_device_fix_up_power(ACPI_COMPANION(dev));
		return -EINVAL;
	}

	/*
	 * Normally we put the reset pin in input / high-impedance mode to save
	 * power. But some x86/ACPI boards don't have a pull-up, so for the ACPI
	 * case, leave the pin as is. This results in the pin not being touched
	 * at all on x86/ACPI boards, except when needed for error-recover.
	 */
	ts->gpiod_rst_flags = GPIOD_ASIS;

	return devm_acpi_dev_add_driver_gpios(dev, gpio_mapping);
}
#else
static int goodix_add_acpi_gpio_mappings(struct goodix_ts_data *ts)
{
	return -EINVAL;
}
#endif /* CONFIG_X86 && CONFIG_ACPI */

/**
 * goodix_get_gpio_config - 从 ACPI/DT 获取 GPIO 配置（中断和复位引脚）
 * @ts: Goodix 触摸数据结构指针
 *
 * 该函数负责从设备固件（设备树或 ACPI）中获取中断（INT）和复位（RST）两个
 * GPIO 描述符，并设置相应的引脚访问方法（irq_pin_access_method）。
 *
 * 复位引脚默认被请求为输入模式（GPIOD_IN），以保持高阻态省电，只有在需要复位时
 * 才由驱动临时切换为输出。中断引脚固定为输入（因为它是来自触摸芯片的中断信号）。
 *
 * 对于 ACPI 系统，如果直接通过名称获取 GPIO 失败，会尝试通过添加 GPIO 映射表
 * 的方式重新获取一次，以兼容缺少名称索引的 ACPI 表。
 *
 * 返回: 0 表示成功，负值表示获取 GPIO 或电源调节器失败。
 */
static int goodix_get_gpio_config(struct goodix_ts_data *ts)
{
	struct device *dev;
	struct gpio_desc *gpiod;
	bool added_acpi_mappings = false;   // 是否已尝试添加 ACPI GPIO 映射

	if (!ts->client)
		return -EINVAL;
	dev = &ts->client->dev;
	goodix_trace(dev, "get_gpio_config: start\n");

	/*
	 * 默认将复位引脚请求为输入（高阻态）。
	 * 这样不在复位期间时，引脚不会驱动电平，既省电又避免总线冲突。
	 * ts->gpiod_rst_flags 将在 devm_gpiod_get_optional 中作为 flags 参数使用。
	 */
	ts->gpiod_rst_flags = GPIOD_IN;

	/* 获取模拟 2.8V 电源调节器（AVDD28） */
	ts->avdd28 = devm_regulator_get(dev, "AVDD28");
	if (IS_ERR(ts->avdd28))
		return dev_err_probe(dev, PTR_ERR(ts->avdd28), "Failed to get AVDD28 regulator\n");
	goodix_trace(dev, "get_gpio_config: AVDD28 regulator ready\n");

	/* 获取 IO 电源调节器（VDDIO） */
	ts->vddio = devm_regulator_get(dev, "VDDIO");
	if (IS_ERR(ts->vddio))
		return dev_err_probe(dev, PTR_ERR(ts->vddio), "Failed to get VDDIO regulator\n");
	goodix_trace(dev, "get_gpio_config: VDDIO regulator ready\n");

retry_get_irq_gpio:
	/*
	 * 获取中断 GPIO，con_id 为 GOODIX_GPIO_INT_NAME（通常为 "irq"）。
	 * 使用 devm_gpiod_get_optional 允许该 GPIO 不存在（返回 NULL），
	 * 并默认将其方向设为输入（GPIOD_IN）。
	 * 设备树中对应的属性为 "irq-gpios"。
	 */
	gpiod = devm_gpiod_get_optional(dev, GOODIX_GPIO_INT_NAME, GPIOD_IN);
	if (IS_ERR(gpiod))
		return dev_err_probe(dev, PTR_ERR(gpiod), "Failed to get %s GPIO\n",
				     GOODIX_GPIO_INT_NAME);
	goodix_trace(dev, "get_gpio_config: INT gpio %s\n", gpiod ? "present" : "absent");

	/*
	 * ACPI 特殊情况：如果通过名称没有找到 GPIO，并且设备有 ACPI 伴生，
	 * 且尚未尝试过添加映射，则调用 goodix_add_acpi_gpio_mappings 添加
	 * 自定义的 name -> index 映射，然后跳回 retry 标签重新获取。
	 * 这是为了兼容那些没有提供 _DSD 或 GPIO 名称索引的旧 ACPI 表。
	 */
	if (!gpiod && has_acpi_companion(dev) && !added_acpi_mappings) {
		added_acpi_mappings = true;
		if (goodix_add_acpi_gpio_mappings(ts) == 0)
			goto retry_get_irq_gpio;
	}

	ts->gpiod_int = gpiod;

	/*
	 * 获取复位 GPIO，con_id 为 GOODIX_GPIO_RST_NAME（通常为 "reset"）。
	 * 使用 ts->gpiod_rst_flags 作为默认方向标志，即 GPIOD_IN（输入）。
	 * 设备树中对应的属性为 "reset-gpios"。
	 * 注意：这里获取到的 GPIO 方向是输入，驱动将在需要复位时临时切换。
	 */
	gpiod = devm_gpiod_get_optional(dev, GOODIX_GPIO_RST_NAME, ts->gpiod_rst_flags);
	if (IS_ERR(gpiod))
		return dev_err_probe(dev, PTR_ERR(gpiod), "Failed to get %s GPIO\n",
				     GOODIX_GPIO_RST_NAME);
	goodix_trace(dev, "get_gpio_config: RST gpio %s\n", gpiod ? "present" : "absent");

	ts->gpiod_rst = gpiod;

	/*
	 * 根据中断引脚的访问方式（ACPI GPIO、ACPI Method 或 GPIO）做最后调整
	 */
	switch (ts->irq_pin_access_method) {
	case IRQ_PIN_ACCESS_ACPI_GPIO:
		/*
		 * 该路径表明之前通过 goodix_add_acpi_gpio_mappings 添加了映射。
		 * 如果此时仍未成功获取到中断或复位 GPIO，说明映射无效，
		 * 将访问方法回退为 IRQ_PIN_ACCESS_NONE，后续可能完全依赖 ACPI 事件。
		 */
		if (!ts->gpiod_int || !ts->gpiod_rst)
			ts->irq_pin_access_method = IRQ_PIN_ACCESS_NONE;
		break;
	case IRQ_PIN_ACCESS_ACPI_METHOD:
		/*
		 * ACPI 方法下，中断引脚可能通过其他方式通知（如 GPE），
		 * 仅检查复位 GPIO 是否存在，若无则回退。
		 */
		if (!ts->gpiod_rst)
			ts->irq_pin_access_method = IRQ_PIN_ACCESS_NONE;
		break;
	default:
		/*
		 * 标准 GPIO 方式：如果中断和复位 GPIO 均成功获取，则：
		 * - 在探测阶段执行复位操作
		 * - 标记引脚访问方法为 IRQ_PIN_ACCESS_GPIO
		 */
		if (ts->gpiod_int && ts->gpiod_rst) {
			ts->reset_controller_at_probe = true;
			ts->irq_pin_access_method = IRQ_PIN_ACCESS_GPIO;
		}
	}

	goodix_trace(dev,
		     "get_gpio_config: done irq_method=%u reset_at_probe=%u int=%s rst=%s\n",
		     ts->irq_pin_access_method, ts->reset_controller_at_probe,
		     ts->gpiod_int ? "yes" : "no", ts->gpiod_rst ? "yes" : "no");
	return 0;
}

/**
 * goodix_read_config - 读取触控芯片内部的配置寄存器参数
 * @ts: 触控设备私有数据结构体指针
 *
 * 必须在 probe 阶段调用，用于从芯片寄存器中获取分辨率、最大触点数、中断触发方式等硬件参数
 */
static void goodix_read_config(struct goodix_ts_data *ts)
{
	int x_max, y_max;
	int error;

	/*
	 * 分支判断：是否需要外部加载固件
	 * 对于无内置 Flash 的芯片，驱动必须先下发固件，此时 ts->config 缓冲区里已经预存了配置数据
	 * 芯片本身此时还没有有效配置，因此跳过读寄存器的操作
	 */
	if (!ts->firmware_name) {
		// 通过I2C读取芯片配置区寄存器，从 config_addr 地址开始，读取 config_len 字节到 ts->config 缓冲区
		error = goodix_i2c_read(ts->client, ts->chip->config_addr,
					ts->config, ts->chip->config_len);
		if (error) {
			// 读取失败：使用驱动默认值兜底，保证设备至少能基础工作
			ts->int_trigger_type = GOODIX_INT_TRIGGER;  // 默认中断触发类型
			ts->max_touch_num = GOODIX_MAX_CONTACTS;    // 默认最大支持触点数
			return;
		}
	}

	// 从配置数据中提取中断触发类型：取对应字节的低2位
	ts->int_trigger_type = ts->config[TRIGGER_LOC] & 0x03;
	// 从配置数据中提取最大支持触点数：取对应字节的低4位
	ts->max_touch_num = ts->config[MAX_CONTACTS_LOC] & 0x0f;

	// 读取X、Y轴最大分辨率：16位小端格式，可能存在地址非对齐，因此用get_unaligned_le16安全读取
	x_max = get_unaligned_le16(&ts->config[RESOLUTION_LOC]);
	y_max = get_unaligned_le16(&ts->config[RESOLUTION_LOC + 2]);
	// 读取到有效分辨率时，更新输入子系统的ABS轴最大值（坐标从0开始，因此最大值要减1）
	if (x_max && y_max) {
		input_abs_set_max(ts->input_dev, ABS_MT_POSITION_X, x_max - 1);
		input_abs_set_max(ts->input_dev, ABS_MT_POSITION_Y, y_max - 1);
	}

	// 计算配置数据的校验和，用于后续下发配置时的完整性校验
	ts->chip->calc_config_checksum(ts);
}

/**
 * goodix_read_version - 读取触控芯片的型号ID和固件版本号
 * @ts: 触控设备私有数据结构体指针
 */
static int goodix_read_version(struct goodix_ts_data *ts)
{
	int error;
	u8 buf[6];  // ID寄存器共6字节：前4字节为型号字符串，后2字节为版本号
	char id_str[GOODIX_ID_MAX_LEN + 1];

	goodix_trace(&ts->client->dev, "read_version: start\n");
	// 读取芯片ID寄存器（GOODIX_REG_ID），读取6字节数据
	error = goodix_i2c_read(ts->client, GOODIX_REG_ID, buf, sizeof(buf));
	if (error)
		return error;

	// 提取前4字节作为芯片型号字符串，补字符串结束符
	memcpy(id_str, buf, GOODIX_ID_MAX_LEN);
	id_str[GOODIX_ID_MAX_LEN] = 0;
	// 保存型号字符串到私有数据结构
	strscpy(ts->id, id_str, GOODIX_ID_MAX_LEN + 1);

	// 提取后2字节作为固件版本号，小端格式转换为16位整数
	ts->version = get_unaligned_le16(&buf[4]);

	// 打印芯片型号和版本信息，方便调试确认硬件型号
	dev_info(&ts->client->dev, "ID %s, version: %04x\n", ts->id,
		 ts->version);
	goodix_trace(&ts->client->dev, "read_version: done id=%s version=0x%04x\n",
		     ts->id, ts->version);

	return 0;
}

/**
 * goodix_i2c_test - I2C通信测试，验证芯片是否能正常响应命令
 * @client: I2C客户端设备指针
 *
 * 通过读取芯片ID寄存器的1个字节，快速验证I2C硬件链路是否通畅
 * 芯片刚上电可能未就绪，因此做2次重试
 */
static int goodix_i2c_test(struct i2c_client *client)
{
	int retry = 0;
	int error;
	u8 test;

	goodix_trace(&client->dev, "i2c_test: start\n");
	// 最多重试2次，避免上电时序未完成导致的误判
	while (retry++ < 2) {
		goodix_trace(&client->dev, "i2c_test: attempt %d\n", retry);
		// 读取ID寄存器的第一个字节，验证芯片应答
		error = goodix_i2c_read(client, GOODIX_REG_ID, &test, 1);
		if (!error) {
			goodix_trace(&client->dev, "i2c_test: success on attempt %d\n",
				     retry);
			return 0;  // 读取成功，通信正常
		}

		// 失败后延时20ms再重试，等待芯片内部初始化完成
		msleep(20);
	}

	// 两次重试均失败，返回最终错误码
	goodix_trace(&client->dev, "i2c_test: failed err=%d\n", error);
	return error;
}

/**
 * goodix_configure_dev - 完成设备最终初始化
 * @ts: 触控设备私有数据结构体指针
 *
 * 必须在probe阶段调用，包含带GPIO和不带GPIO设备的通用初始化逻辑
 * 既可以直接从probe调用，也可以从固件加载完成的回调中调用
 */
static int goodix_configure_dev(struct goodix_ts_data *ts)
{
	int error;
	int i;

	goodix_trace(&ts->client->dev, "configure_dev: start\n");

	// 先设置默认参数兜底，防止读取配置失败导致参数为空
	ts->int_trigger_type = GOODIX_INT_TRIGGER;
	ts->max_touch_num = GOODIX_MAX_CONTACTS;

	// 分配一个输入设备结构体（devm托管，驱动卸载时自动释放）
	ts->input_dev = devm_input_allocate_device(&ts->client->dev);
	if (!ts->input_dev) {
		dev_err(&ts->client->dev, "Failed to allocate input device.");
		return -ENOMEM;
	}

	// ---------- 配置输入设备基础信息 ----------
	ts->input_dev->name = "Goodix Capacitive TouchScreen";  // 设备名称，用户态可见
	ts->input_dev->phys = "input/ts";                       // 设备物理路径
	ts->input_dev->id.bustype = BUS_I2C;                    // 总线类型：I2C
	ts->input_dev->id.vendor = 0x0416;                      // 厂商ID（Goodix的USB/输入子系统厂商号）
	// 产品ID：尝试从芯片ID字符串转换，失败则用默认值0x1001
	if (kstrtou16(ts->id, 10, &ts->input_dev->id.product))
		ts->input_dev->id.product = 0x1001;
	ts->input_dev->id.version = ts->version;                // 版本号

	// ---------- 配置实体按键（部分芯片带Home/Windows键） ----------
	ts->input_dev->keycode = ts->keymap;        // 按键映射表
	ts->input_dev->keycodesize = sizeof(ts->keymap[0]); // 单个按键码大小
	ts->input_dev->keycodemax = GOODIX_MAX_KEYS; // 最大按键数量

	// 循环初始化按键能力
	for (i = 0; i < GOODIX_MAX_KEYS; ++i) {
		if (i == 0)
			ts->keymap[i] = KEY_LEFTMETA;  // 第1个按键映射为Windows键（左Meta）
		else
			ts->keymap[i] = KEY_F1 + (i - 1); // 后续按键依次映射为F1、F2...

		// 向输入子系统注册该按键能力
		input_set_capability(ts->input_dev, EV_KEY, ts->keymap[i]);
	}

	// ---------- 配置多点触控ABS轴 ----------
	// 注册X、Y坐标轴
	input_set_capability(ts->input_dev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(ts->input_dev, EV_ABS, ABS_MT_POSITION_Y);
	// 注册触摸宽度、触摸面积轴，用于识别按压力度/接触面积
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

retry_read_config:
	// 读取芯片内置配置，应用到输入设备参数
	goodix_trace(&ts->client->dev, "configure_dev: read controller config\n");
	goodix_read_config(ts);

	// 解析设备树中的触控属性（如触摸屏尺寸、翻转、交换XY轴等），优先级高于芯片内置配置
	touchscreen_parse_properties(ts->input_dev, true, &ts->prop);
	goodix_trace(&ts->client->dev,
		     "configure_dev: trigger=%u max_touch=%u max_x=%u max_y=%u\n",
		     ts->int_trigger_type, ts->max_touch_num,
		     ts->prop.max_x, ts->prop.max_y);

	// 校验核心参数：X/Y最大值、最大触点数不能为0
	if (!ts->prop.max_x || !ts->prop.max_y || !ts->max_touch_num) {
		// 第一次读取失败，且有复位引脚可用时，复位芯片后重试读取
		if (!ts->reset_controller_at_probe &&
		    ts->irq_pin_access_method != IRQ_PIN_ACCESS_NONE) {
			dev_info(&ts->client->dev, "Config not set, resetting controller\n");
			ts->reset_controller_at_probe = true;  // 标记已执行过复位，避免无限重试
			error = goodix_reset(ts);  // 执行硬件复位
			if (error)
				return error;
			goto retry_read_config;  // 跳回重新读取配置
		}
		// 复位后仍失败，打印警告，使用驱动默认值兜底
		dev_err(&ts->client->dev,
			"Invalid config (%d, %d, %d), using defaults\n",
			ts->prop.max_x, ts->prop.max_y, ts->max_touch_num);
		ts->prop.max_x = GOODIX_MAX_WIDTH - 1;
		ts->prop.max_y = GOODIX_MAX_HEIGHT - 1;
		ts->max_touch_num = GOODIX_MAX_CONTACTS;
		// 应用默认参数到输入设备
		input_abs_set_max(ts->input_dev,
				  ABS_MT_POSITION_X, ts->prop.max_x);
		input_abs_set_max(ts->input_dev,
				  ABS_MT_POSITION_Y, ts->prop.max_y);
	}

	// ---------- x86平台兼容补丁（DMI匹配，ARM设备树平台不会触发） ----------
	// 补丁1：部分设备使用9字节的非标准触点报告格式
	if (dmi_check_system(nine_bytes_report)) {
		ts->contact_size = 9;
		dev_dbg(&ts->client->dev,
			"Non-standard 9-bytes report format quirk\n");
	}
	// 补丁2：部分屏幕X轴坐标默认反向
	if (dmi_check_system(inverted_x_screen)) {
		ts->prop.invert_x = true;
		dev_dbg(&ts->client->dev,
			"Applying 'inverted x screen' quirk\n");
	}

	// ---------- 初始化多点触控槽位（MT协议B类） ----------
	// INPUT_MT_DIRECT：直接触控设备（触摸屏，区别于触控板）
	// INPUT_MT_DROP_UNUSED：自动丢弃无效触点，简化上层处理
	error = input_mt_init_slots(ts->input_dev, ts->max_touch_num,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		dev_err(&ts->client->dev,
			"Failed to initialize MT slots: %d", error);
		return error;
	}

	// 将私有数据指针存入输入设备，方便中断回调中获取
	input_set_drvdata(ts->input_dev, ts);

	// ---------- 中断/轮询模式选择 ----------
	// 如果设备树没有配置中断引脚，则自动回退到轮询模式
	if (!ts->client->irq) {
		// 设置轮询回调函数，定期主动读取触摸数据
		error = input_setup_polling(ts->input_dev, goodix_ts_work_i2c_poll);
		if (error) {
			dev_err(&ts->client->dev,
				 "could not set up polling mode, %d\n", error);
			return error;
		}
		// 设置轮询间隔，默认几十毫秒一次
		input_set_poll_interval(ts->input_dev, GOODIX_POLL_INTERVAL_MS);
	}

	// ---------- 注册输入设备到内核 ----------
	// 注册完成后，用户态就能看到/dev/input/eventX设备
	error = input_register_device(ts->input_dev);
	if (error) {
		dev_err(&ts->client->dev,
			"Failed to register input device: %d", error);
		return error;
	}

	/*
	 * 笔输入设备创建：放在中断申请之前
	 * 利用devm的释放顺序：先申请的后释放，保证中断关闭后再释放笔设备
	 * 由于无法从寄存器直接判断是否支持笔输入，因此延迟到第一次笔事件时再正式注册
	 */
	error = goodix_create_pen_input(ts);
	if (error)
		return error;

	// ---------- 申请中断 ----------
	// 根据中断触发类型，获取对应的中断标志，加上IRQF_ONESHOT（线程化中断专用）
	ts->irq_flags = goodix_irq_flags[ts->int_trigger_type] | IRQF_ONESHOT;
	goodix_trace(&ts->client->dev, "configure_dev: request irq flags=0x%lx irq=%d\n",
		     ts->irq_flags, ts->client->irq);
	error = goodix_request_irq(ts);
	if (error) {
		dev_err(&ts->client->dev, "request IRQ failed: %d\n", error);
		return error;
	}

	goodix_trace(&ts->client->dev, "configure_dev: done\n");
	return 0;
}

/**
 * goodix_disable_regulators - devm托管的电源清理函数
 * @arg: 上下文指针，即 goodix_ts_data 结构体
 *
 * 由 devm_add_action_or_reset 注册，驱动卸载、probe出错时自动调用
 * 保证电源被正确关闭，避免硬件悬空耗电
 */
static void goodix_disable_regulators(void *arg)
{
	struct goodix_ts_data *ts = arg;

	// 依次关闭IO电源和模拟电源
	regulator_disable(ts->vddio);
	regulator_disable(ts->avdd28);
}

/**
 * goodix_ts_probe - I2C驱动probe入口函数
 * @client: I2C客户端设备指针，由I2C核心在匹配设备后传入
 *
 * 执行完整的硬件初始化、资源申请、设备注册流程
 */
static int goodix_ts_probe(struct i2c_client *client)
{
	struct goodix_ts_data *ts;
	int error;

	dev_dbg(&client->dev, "I2C Address: 0x%02x\n", client->addr);
	goodix_trace(&client->dev, "probe: start addr=0x%02x irq=%d\n",
		     client->addr, client->irq);

	// 第一步：检查I2C控制器是否支持标准I2C协议
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C check functionality failed.\n");
		return -ENXIO;
	}

	// 第二步：分配设备私有数据结构体（devm托管，自动释放）
	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	// 初始化私有数据基础成员
	ts->client = client;
	i2c_set_clientdata(client, ts);  // 将私有数据存入I2C客户端，方便后续回调获取
	ts->contact_size = GOODIX_CONTACT_SIZE; // 默认触点报告大小

	// 第三步：从设备树解析GPIO配置（复位引脚、中断引脚）
	error = goodix_get_gpio_config(ts);
	if (error)
		return error;
	goodix_trace(&client->dev, "probe: gpio config ready\n");

	// 第四步：芯片上电，使能两路电源
	error = regulator_enable(ts->avdd28);  // 2.8V模拟电源
	if (error) {
		dev_err(&client->dev,
			"Failed to enable AVDD28 regulator: %d\n",
			error);
		return error;
	}
	goodix_trace(&client->dev, "probe: AVDD28 enabled\n");

	error = regulator_enable(ts->vddio);   // IO口电源
	if (error) {
		dev_err(&client->dev,
			"Failed to enable VDDIO regulator: %d\n",
			error);
		regulator_disable(ts->avdd28); // 失败回滚，关闭已打开的电源
		return error;
	}
	goodix_trace(&client->dev, "probe: VDDIO enabled\n");

	// 注册电源自动清理回调：驱动卸载/出错时自动关闭电源
	error = devm_add_action_or_reset(&client->dev,
					 goodix_disable_regulators, ts);
	if (error)
		return error;
	goodix_trace(&client->dev, "probe: regulator cleanup registered\n");

reset:
	// 如果标记了需要复位，执行芯片硬件复位
	if (ts->reset_controller_at_probe) {
		goodix_trace(&client->dev, "probe: perform controller reset\n");
		error = goodix_reset(ts);
		if (error)
			return error;
	}

	// 第五步：I2C通信测试，验证硬件链路通畅
	goodix_trace(&client->dev, "probe: run i2c test\n");
	error = goodix_i2c_test(client);
	if (error) {
		// 第一次通信失败，且有复位引脚可用时，复位后重试一次
		if (!ts->reset_controller_at_probe &&
		    ts->irq_pin_access_method != IRQ_PIN_ACCESS_NONE) {
			ts->reset_controller_at_probe = true;
			goodix_trace(&client->dev, "probe: i2c test failed, retry with reset\n");
			goto reset;
		}
		// 重试后仍失败，返回通信错误，probe失败
		dev_err(&client->dev, "I2C communication failure: %d\n", error);
		return error;
	}

	// // 第六步：检查是否需要加载外部固件
	// error = goodix_firmware_check(ts);
	// if (error)
	// 	return error;

	// 第七步：读取芯片型号和版本
	goodix_trace(&client->dev, "probe: read chip version\n");
	error = goodix_read_version(ts);
	if (error)
		return error;

	// 第八步：根据芯片ID匹配对应的芯片参数表（配置地址、长度、校验函数等）
	ts->chip = goodix_get_chip_data(ts->id);
	goodix_trace(&client->dev, "probe: matched chip data for id=%s\n", ts->id);

	// 第九步：外部固件/配置加载路径已删除，直接同步完成设备初始化注册
	goodix_trace(&client->dev, "probe: configure input device\n");
	error = goodix_configure_dev(ts);
	if (error)
		return error;

	goodix_trace(&client->dev, "probe: done\n");
	return 0;
}

static void goodix_ts_remove(struct i2c_client *client)
{
}

/**
 * goodix_suspend - 使触摸芯片进入休眠（挂起）状态
 * @dev: 设备结构体指针（通常是 i2c_client->dev）
 *
 * 该函数在系统挂起或屏幕关闭时调用，用于降低功耗。
 * 流程：释放中断 → 将 INT 引脚置为输出低电平并保持 5ms →
 * 通过 I2C 发送屏幕关闭命令 → 延迟 58ms 确保芯片进入休眠。
 * 休眠后，触摸芯片停止报点，INT 引脚保持低电平（或高阻？）
 *
 * 返回: 0 成功，负值表示失败。
 */
static int goodix_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct goodix_ts_data *ts = i2c_get_clientdata(client);
	int error;

	goodix_trace(dev, "suspend: start\n");
	/*
	 * 如果没有 GPIO 引脚可用（IRQ_PIN_ACCESS_NONE），我们无法通过
	 * 操作 INT 引脚进入休眠，只能简单地禁用中断。
	 */
	if (ts->irq_pin_access_method == IRQ_PIN_ACCESS_NONE) {
		goodix_trace(dev, "suspend: no gpio access, disable irq only\n");
		goodix_disable_irq(ts);
		return 0;
	}

	/*
	 * 释放中断处理函数，因为接下来 INT 引脚将被切换为输出模式，
	 * 不再用作中断输入。如果保留中断，引脚切换可能导致虚假中断。
	 */
	goodix_free_irq(ts);
	goodix_trace(dev, "suspend: freed irq before driving INT\n");

	/* 如有必要，保存基准参考数据（用于校准） */
	goodix_save_bak_ref(ts);
	goodix_trace(dev, "suspend: saved backup reference if needed\n");

	/*
	 * 将中断引脚（INT）切换为输出，并输出低电平，持续 5ms。
	 * 这一步是 Goodix 休眠协议的一部分：将 INT 拉低通知芯片即将进入休眠。
	 * 注意：goodix_irq_direction_output() 现在输出的是物理低电平。
	 */
	goodix_trace(dev, "suspend: step1 drive INT low\n");
	error = goodix_irq_direction_output(ts, 0);
	if (error) {
		/* 如果切换输出失败，恢复中断并返回错误 */
		goodix_request_irq(ts);
		return error;
	}

	usleep_range(5000, 6000);  /* 保持低电平 5~6ms */
	goodix_trace(dev, "suspend: step2 INT low held for 5-6ms\n");

	/*
	 * 通过 I2C 向寄存器 GOODIX_REG_COMMAND 写入命令 GOODIX_CMD_SCREEN_OFF，
	 * 正式通知触摸芯片进入屏幕关闭/休眠模式。
	 */
	goodix_trace(dev, "suspend: step3 send screen-off command\n");
	error = goodix_i2c_write_u8(ts->client, GOODIX_REG_COMMAND,
				    GOODIX_CMD_SCREEN_OFF);
	if (error) {
		/* 如果 I2C 写入失败，将 INT 切换回输入并重新请求中断 */
		goodix_irq_direction_input(ts);
		goodix_request_irq(ts);
		return -EAGAIN;
	}

	/*
	 * 数据手册规定：发送屏幕关闭命令后，至少要等待 58ms 才能尝试唤醒。
	 * 这里延迟 58ms，确保芯片稳定进入休眠，避免误唤醒。
	 */
	msleep(58);
	goodix_trace(dev, "suspend: done\n");
	return 0;
}

/**
 * goodix_resume - 唤醒触摸芯片（从休眠状态恢复）
 * @dev: 设备结构体指针
 *
 * 函数通过操作 INT 引脚输出高电平脉冲来唤醒触摸芯片，然后读取配置版本，
 * 如果版本不匹配则执行复位和重新发送配置，最后重新注册中断处理函数。
 *
 * 返回: 0 成功，负值表示失败。
 */
static int goodix_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct goodix_ts_data *ts = i2c_get_clientdata(client);
	u8 config_ver;
	int error;

	goodix_trace(dev, "resume: start\n");
	/* 无 GPIO 引脚时，仅使能中断即可 */
	if (ts->irq_pin_access_method == IRQ_PIN_ACCESS_NONE) {
		goodix_trace(dev, "resume: no gpio access, enable irq only\n");
		goodix_enable_irq(ts);
		return 0;
	}

	/*
	 * 将 INT 引脚切换为输出，并输出高电平，持续 2~5ms。
	 * 根据 Goodix 协议，INT 引脚上的上升沿会唤醒休眠的芯片。
	 * 注意：goodix_irq_direction_output() 现在输出的是物理高电平。
	 */
	goodix_trace(dev, "resume: step1 drive INT high for wakeup\n");
	error = goodix_irq_direction_output(ts, 1);
	if (error)
		return error;

	usleep_range(2000, 5000);  /* 保持高电平 2~5ms */
	goodix_trace(dev, "resume: step2 wake pulse held for 2-5ms\n");

	/*
	 * 执行 INT 引脚同步操作：通常是将 INT 引脚重新配置为输入功能，
	 * 并确保电平稳定，为接下来的中断检测做好准备。
	 */
	goodix_trace(dev, "resume: step3 run INT sync\n");
	error = goodix_int_sync(ts);
	if (error)
		return error;

	/*
	 * 读取触摸芯片的配置版本寄存器，与驱动中保存的版本进行比较。
	 * 如果芯片因掉电等原因丢失了配置，版本号会不匹配。
	 */
	goodix_trace(dev, "resume: step4 read config version\n");
	error = goodix_i2c_read(ts->client, ts->chip->config_addr,
				&config_ver, 1);
	if (!error && config_ver != ts->config[0])
		dev_info(dev, "Config version mismatch %d != %d, resetting controller\n",
			 config_ver, ts->config[0]);

	/*
	 * 如果读取失败或版本不一致，执行硬件复位并重新发送配置文件。
	 * 复位操作会通过 reset_gpios 引脚发出复位信号，然后完整重配芯片。
	 */
	if (error != 0 || config_ver != ts->config[0]) {
		goodix_trace(dev,
			     "resume: step5 config mismatch/read error, reset and resend cfg\n");
		error = goodix_reset(ts);
		if (error)
			return error;

		error = goodix_send_cfg(ts, ts->config, ts->chip->config_len);
		if (error)
			return error;
	}

	/*
	 * 重新注册中断处理函数，使触摸芯片可以正常报点。
	 */
	goodix_trace(dev, "resume: step6 request irq again\n");
	error = goodix_request_irq(ts);
	if (error)
		return error;

	goodix_trace(dev, "resume: done\n");
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(goodix_pm_ops, goodix_suspend, goodix_resume);

static const struct i2c_device_id goodix_ts_id[] = {
	{ "GDIX1001:00" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, goodix_ts_id);

#ifdef CONFIG_ACPI
static const struct acpi_device_id goodix_acpi_match[] = {
	{ "GDIX1001", 0 },
	{ "GDIX1002", 0 },
	{ "GDIX1003", 0 },
	{ "GDX9110", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, goodix_acpi_match);
#endif

#ifdef CONFIG_OF
static const struct of_device_id goodix_of_match[] = {
	{ .compatible = "goodix,gt1151" },
	{ .compatible = "goodix,gt1158" },
	{ .compatible = "goodix,gt5663" },
	{ .compatible = "goodix,gt5688" },
	{ .compatible = "goodix,gt911" },
	{ .compatible = "goodix,gt9110" },
	{ .compatible = "goodix,gt912" },
	{ .compatible = "goodix,gt9147" },
	{ .compatible = "goodix,gt917s" },
	{ .compatible = "goodix,gt927" },
	{ .compatible = "goodix,gt9271" },
	{ .compatible = "goodix,KD-gt928" },
	{ .compatible = "goodix,gt9286" },
	{ .compatible = "goodix,gt967" },
	{ }
};
MODULE_DEVICE_TABLE(of, goodix_of_match);
#endif

static struct i2c_driver goodix_ts_driver = {
	.probe = goodix_ts_probe,
	.remove = goodix_ts_remove,
	.id_table = goodix_ts_id,
	.driver = {
		.name = "Goodix-TS",
		.acpi_match_table = ACPI_PTR(goodix_acpi_match),
		.of_match_table = of_match_ptr(goodix_of_match),
		.pm = pm_sleep_ptr(&goodix_pm_ops),
	},
};
module_i2c_driver(goodix_ts_driver);

MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_AUTHOR("Bastien Nocera <hadess@hadess.net>");
MODULE_DESCRIPTION("Goodix touchscreen driver");
MODULE_LICENSE("GPL v2");
