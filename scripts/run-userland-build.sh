#!/bin/sh
# wrapper: avoid Git-Bash munging of /mnt paths when passed as wsl -e args
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
sh /mnt/f/Linux/Parlz/scripts/build-userland.sh
