// SPDX-License-Identifier: GPL-2.0
/*
 * Parlz - x86 identity layer.  Adds a banner + boot hook so user-space
 * and the console can detect that they are running under Parlz.
 */

#include <linux/init.h>
#include <linux/printk.h>
#include <linux/parlz.h>

const char *parlz_banner;
const char *parlz_release = PARLZ_RELEASE_ID;

void __init parlz_boot_init(void)
{
	parlz_banner = "Parlz " PARLZ_VERSION;
	pr_err("%s on x86_64 (Linux 7.2.5 base)\n", parlz_banner);
	/* 第二行是发布号; 同一个串也被接进 UTS_RELEASE(CONFIG_LOCALVERSION),
	 * 所以 /proc/version、uname -r、guest 的 /etc/parlz-release 三处一致。 */
	pr_err("Parlz release %s\n", parlz_release);
}
