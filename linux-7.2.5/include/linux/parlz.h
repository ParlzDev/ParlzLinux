/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Parlz - kernel-wide identity surface.
 */
#ifndef _LINUX_PARLZ_H
#define _LINUX_PARLZ_H

#include <linux/types.h>
#include <linux/parlz-release.h>

extern const char *parlz_banner;

/* 本次发布的完整版本串(时区+构建时间+构建人+阶段.大.小), 定义在
 * arch/x86/kernel/parlz.c; 与 /proc/version 里 UTS_RELEASE 的尾部是同一个串。 */
extern const char *parlz_release;

static inline int parlz_running(void)
{
	return parlz_banner != NULL;
}

void __init parlz_boot_init(void);

#endif /* _LINUX_PARLZ_H */
