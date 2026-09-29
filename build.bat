@echo off
REM build.bat - Windows 侧全量构建 Parlz(用户空间 + 内核 + 引导镜像 + ISO)
REM 依次跑 build-userland → build-kernel(末尾生成引导镜像)→ build-iso
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/build-userland.sh
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/build-kernel.sh
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/build-iso.sh
