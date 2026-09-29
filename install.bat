@echo off
REM install.bat - Windows 侧进系统里自己装到磁盘(交互式)
REM 进去后是个 shell, 自己敲:  install  然后  reboot
REM 也可以改用 run-iso.sh 走 ISO 全自动安装
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-install.sh
