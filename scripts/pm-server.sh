#!/bin/sh
# pm-server.sh - pm feed 服务器端(宿主 WSL 起静态 HTTP, 托管 pm-repo)。
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/pm-server.sh"
#       (改端口: PM_PORT=9000 ...; 端口与 guest 里 feeds.conf 的端口要一致)
#
# 起 python3 http.server 把 /home/jgzyes/pm-repo/ 挂 0.0.0.0:8765。
# guest 里 QEMU user NAT(10.0.2.15)访问宿主 IP 10.0.2.2:8765 即命中
# (QEMU user NAT 把 10.0.2.2 映射回宿主回环/本地)。
# 生产环境换 nginx/caddy 托管同一目录即可, 目录布局不变:
#   <base>/
#     Packages          # 索引: 行 "包名 版本 包文件名 大小bytes"
#     gcc-15.2.pm
#     clang-llvm-21.1.pm
#
# 验证(宿主侧): curl -s http://127.0.0.1:8765/Packages | head
# 验证(guest):  写 /etc/pm/feeds.conf = http://10.0.2.2:8765/pm-repo?
#   实际 base 由 feeds.conf 一行给出; 本脚本目录是 pm-repo, 直接作 base:
#   /etc/pm/feeds.conf 写  http://10.0.2.2:8765
#   (因为 http.server 根 = pm-repo 目录, 访问 /Packages 与 /gcc-15.2.pm)
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

REPO=${PM_REPO:-/home/jgzyes/pm-repo}
PORT="${PM_PORT:-8765}"
[ -d "$REPO" ] || { echo "缺 $REPO(先跑 build-pm-feed.sh 或 build-pm-packages.sh)"; exit 1; }
[ -f "$REPO/Packages" ] || echo "提醒: $REPO 里没有 Packages 索引, guest 的 pm available 会看不到包"

# 已有在跑则提示
if pgrep -f "http.server $PORT" > /dev/null 2>&1; then
  echo "pm-server: 端口 $PORT 已在跑, 不再起"
  exit 0
fi

cd "$REPO"
echo "pm-server: 托管 $REPO @ 0.0.0.0:$PORT (Ctrl-C 停)"
echo "guest feeds.conf: http://10.0.2.2:$PORT"
# 后台 python http.server(不跟随符号链接, 足够托管 .pm 大文件)
exec python3 -m http.server "$PORT" --bind 0.0.0.0
