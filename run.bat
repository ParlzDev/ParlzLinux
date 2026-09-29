@echo off
REM run.bat - Windows 侧从已安装的磁盘启动 Parlz
REM 前置: 先跑 install.bat(或 scripts/run-iso.sh)把系统装到磁盘
REM 退出 QEMU: Ctrl-A 然后 X
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-disk.sh
