#!/bin/sh
# scripts/git-init-repo.sh —— 建（或补齐）自托管仓库 git/parlz.git
#
# 为什么不用 .git：工作区要保持"不是一个仓库"的状态（别的工具链按目录树在用），
# 所以仓库数据放 git/parlz.git，操作一律显式给 --git-dir / --work-tree。
# 也不碰使用者的全局 git 配置：作者信息用 -c 逐条命令覆盖。
#
#   sh scripts/git-init-repo.sh          # 没有就建，有就 add + 该提交就提交
#   MSG="..." sh scripts/git-init-repo.sh
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GITD="$ROOT/git/parlz.git"
G="git --git-dir=$GITD --work-tree=$ROOT -c core.autocrlf=false"

# NTFS 上没有 Unix 执行位；filemode=false 免得每次 add 都报 644→755 的假改动。
# 行尾：core.autocrlf=false 是关键 —— 内核源码树的 LF 绝不能被 Windows git 改写，
# 否则哈希全变、diff 全是噪声。作者信息只用 -c 传，不落任何配置文件。
if [ ! -d "$GITD" ]; then
  mkdir -p "$ROOT/git"
  git init --bare --initial-branch=main "$GITD" >/dev/null
  git --git-dir="$GITD" config core.bare false
  git --git-dir="$GITD" config core.filemode false
  git --git-dir="$GITD" config core.autocrlf false
  git --git-dir="$GITD" config gc.auto 0
  echo "已建裸库 $GITD（分支 main）"
fi

echo "== 索引工作区（按 .gitignore；9.6 万个文件，慢就让它慢）=="
$G add -A .

if $G diff --cached --quiet; then
  echo "== 没有待提交的改动 =="
else
  MSG=${MSG:-"parlz: 首次入库 —— 就地改造的 linux-7.2.5 内核树、userland、scripts、官网与 NTCLKS 支路"}
  echo "== 提交 =="
  $G -c user.name="jgzyes" -c user.email="jgzyes@parlz.com" commit -q -m "$MSG"
fi

echo "== 打包（网站导出与 clone 都快）=="
# 不用 --aggressive：9.6 万个文件重算 delta 是单线程的，能跑到十几分钟；默认线程的 repack 就够
$G repack -a -d -q || echo "（repack 跳过）"

echo "== 结果 =="
$G log --oneline -3
$G count-objects -vH
echo "对象库：$(du -sh "$GITD" 2>/dev/null | cut -f1)"
