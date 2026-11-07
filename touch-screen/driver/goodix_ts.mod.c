#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

MODULE_INFO(intree, "Y");



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xf5528973, "regulator_enable" },
	{ 0x6466d640, "input_copy_abs" },
	{ 0xc6d09aa9, "release_firmware" },
	{ 0x7e6885f3, "devm_request_threaded_irq" },
	{ 0x4a078b18, "devm_kmalloc" },
	{ 0x52c5c991, "__kmalloc_noprof" },
	{ 0x656e4a6e, "snprintf" },
	{ 0x6c25468c, "input_mt_sync_frame" },
	{ 0x608741b5, "__init_swait_queue_head" },
	{ 0x3ecbc3ea, "request_firmware" },
	{ 0x945a5898, "input_mt_report_slot_state" },
	{ 0xa927467f, "devm_input_allocate_device" },
	{ 0x4829a47e, "memcpy" },
	{ 0x37a0cba, "kfree" },
	{ 0xc3055d20, "usleep_range_state" },
	{ 0xaff4a317, "devm_gpiod_get_optional" },
	{ 0xcbcaf2f0, "input_setup_polling" },
	{ 0x4af6ddf0, "kstrtou16" },
	{ 0xf5edea2e, "___ratelimit" },
	{ 0x9f06c375, "input_register_device" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xc28b9b34, "device_property_read_u32_array" },
	{ 0x2c571d4a, "devm_regulator_get" },
	{ 0x22f76fc3, "_dev_info" },
	{ 0x476b165a, "sized_strscpy" },
	{ 0x18927b, "i2c_register_driver" },
	{ 0x80456726, "_dev_err" },
	{ 0x66658c92, "request_firmware_nowait" },
	{ 0x57f45eea, "gpiod_direction_output_raw" },
	{ 0x160f91fc, "input_mt_init_slots" },
	{ 0xe562898d, "gpiod_direction_input" },
	{ 0xa53da7ca, "device_property_read_string" },
	{ 0xe795e9f0, "input_set_capability" },
	{ 0xdcb764ad, "memset" },
	{ 0xd4835ef8, "dmi_check_system" },
	{ 0xc5acba0f, "input_set_poll_interval" },
	{ 0x25974000, "wait_for_completion" },
	{ 0x57a43825, "input_event" },
	{ 0x93d6dd8c, "complete_all" },
	{ 0xe93db5ea, "input_set_abs_params" },
	{ 0xe2d5255a, "strcmp" },
	{ 0x15ba50a6, "jiffies" },
	{ 0x53f2e42e, "touchscreen_report_pos" },
	{ 0xa0469fa6, "input_alloc_absinfo" },
	{ 0xcac7a934, "i2c_transfer" },
	{ 0x8a46fe01, "regulator_disable" },
	{ 0xed72fda2, "dev_err_probe" },
	{ 0x5e53d8c3, "__devm_add_action" },
	{ 0xbbf9d1c, "i2c_del_driver" },
	{ 0xbbd3857f, "touchscreen_parse_properties" },
	{ 0xafd11717, "gpiod_direction_output" },
	{ 0xf9a482f9, "msleep" },
	{ 0x474e54d2, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("of:N*T*Cgoodix,gt1151");
MODULE_ALIAS("of:N*T*Cgoodix,gt1151C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt1158");
MODULE_ALIAS("of:N*T*Cgoodix,gt1158C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt5663");
MODULE_ALIAS("of:N*T*Cgoodix,gt5663C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt5688");
MODULE_ALIAS("of:N*T*Cgoodix,gt5688C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt911");
MODULE_ALIAS("of:N*T*Cgoodix,gt911C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt9110");
MODULE_ALIAS("of:N*T*Cgoodix,gt9110C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt912");
MODULE_ALIAS("of:N*T*Cgoodix,gt912C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt9147");
MODULE_ALIAS("of:N*T*Cgoodix,gt9147C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt917s");
MODULE_ALIAS("of:N*T*Cgoodix,gt917sC*");
MODULE_ALIAS("of:N*T*Cgoodix,gt927");
MODULE_ALIAS("of:N*T*Cgoodix,gt927C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt9271");
MODULE_ALIAS("of:N*T*Cgoodix,gt9271C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt928");
MODULE_ALIAS("of:N*T*Cgoodix,gt928C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt9286");
MODULE_ALIAS("of:N*T*Cgoodix,gt9286C*");
MODULE_ALIAS("of:N*T*Cgoodix,gt967");
MODULE_ALIAS("of:N*T*Cgoodix,gt967C*");
MODULE_ALIAS("i2c:GDIX1001:00");

MODULE_INFO(srcversion, "1EB8FC68C6DDF78D09F16CA");
