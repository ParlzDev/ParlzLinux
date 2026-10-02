#!/bin/sh
# scripts/web-git-export.sh —— 把 git/parlz.git 导成官网仓库浏览器用的静态数据（web/git/）
#
# 改了源码并提交之后跑一次；产物 web/git/ 要跟着站点一起部署
# （blobs.bin 是最大的一份，其它是索引与提交详情）。
#
# 可调环境变量：
#   GIT_MAX_TEXT=524288   单文件超过这么多字节就不进正文库（默认 512 KB）
#   GIT_DIR / GIT_OUT     仓库与输出目录
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$ROOT/git/parlz.git" ] || { echo "没有 git/parlz.git —— 先 sh scripts/git-init-repo.sh"; exit 1; }
command -v node >/dev/null || { echo "需要 node（宿主上直接跑也行）"; exit 1; }
cd "$ROOT"
node scripts/web-git-export.js
