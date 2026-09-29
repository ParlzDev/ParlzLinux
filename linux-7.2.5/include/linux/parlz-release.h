/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Parlz 发布号 —— 单一来源, 由 scripts/make-release.sh 生成(手工改会被下次发布覆盖)。
 *
 * 发布号格式:  时区 + 构建时间 + 构建人 + 阶段.大版本.小版本[-F\V 或 -UN\V]
 *   PARLZ_RELEASE_ID = "CST+0800+20260927-190002+jgzyes@parlz.com+rc.0.1-F\V"
 * 内核把同一个串接进 UTS_RELEASE(CONFIG_LOCALVERSION = "-<发布号>", 反斜杠
 * 在 C 字符串里已双写转义), 于是 /proc/version 与 uname -r 里就是它的原文。
 */
#ifndef _LINUX_PARLZ_RELEASE_H
#define _LINUX_PARLZ_RELEASE_H

#define PARLZ_VERSION    "0.1.0"
#define PARLZ_MAJOR      "0"
#define PARLZ_MINOR      "1"
#define PARLZ_STAGE      "rc"                  /* alpha / beta / rc / release */
#define PARLZ_BUILDER    "jgzyes@parlz.com"
#define PARLZ_BUILD_TZ   "CST+0800"                    /* date +%Z%z */
#define PARLZ_BUILD_TIME "20260927-190002"                     /* date +%Y%m%d-%H%M%S */
#define PARLZ_RELEASE_ID "CST+0800+20260927-190002+jgzyes@parlz.com+rc.0.1-F\\V"

#endif /* _LINUX_PARLZ_RELEASE_H */
