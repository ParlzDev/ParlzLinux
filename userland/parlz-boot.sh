#!/bin/sh
# parlz-boot.sh - busybox-init ::sysinit 启动器。
# busybox-init 直接 exec 本文件(shebang /bin/sh = shx 判别器:
# 脚本模式走 /bin/bash)。逻辑在 parlz-boot-body.sh, 这里只 exec。
# 注意: 即使 shx 误路由到 parlz-sh, 单行 exec 也能正常 exec 出去。
exec /bin/bash /usr/local/bin/parlz-boot-body.sh
