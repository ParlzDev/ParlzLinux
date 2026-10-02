# CMakeLists.txt - Parlz 用户空间 (CMake + GCC)
cmake_minimum_required(VERSION 3.16)
project(parlz-userland C)

# 静态链接 + 关 PIE，保证 initramfs 里的二进制自包含。
# Ubuntu 的 gcc 默认生成 PIE，-fno-pie 必须同时给编译器和链接器。
add_compile_options(-O2 -fno-pie -fno-stack-protector -D_GNU_SOURCE)
add_link_options(-static -fno-pie)
set(CMAKE_C_STANDARD 99)

# 产物统一放到 build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- curl/wget 的 TLS 后端(OpenSSL 静态库,由 build-userland.sh 传入路径)---
# PARLZ_OPENSSL_PREFIX 指向 make install_sw 的 stage(含 include/ 与 lib/)。
# 未提供时 curl/wget 退回明文 HTTP(编译期开关 PARLZ_HAVE_OPENSSL)。
set(PARLZ_OPENSSL_PREFIX "" CACHE PATH "OpenSSL 静态安装前缀")
if(PARLZ_OPENSSL_PREFIX AND EXISTS "${PARLZ_OPENSSL_PREFIX}/lib/libssl.a"
   AND EXISTS "${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a")
  set(PARLZ_HAVE_OPENSSL 1)
else()
  set(PARLZ_HAVE_OPENSSL 0)
  # 缺库时把 prefix 置空: 各目标 if(PARLZ_HAVE_OPENSSL) 的 include/link
  # 全不生效, 编译期开关退回无 TLS(pweb/pms/curl/wget 纯 HTTP)。
  set(PARLZ_OPENSSL_PREFIX "")
endif()
message(STATUS "OpenSSL prefix: ${PARLZ_OPENSSL_PREFIX} (HAVE=${PARLZ_HAVE_OPENSSL})")

# --- 基础命令 ---
add_executable(init   init.c)
add_executable(login  login.c parlzauth.c)
# login/user 存 $6$(SHA-512)口令散列,用 libxcrypt 的 crypt_r —— 静态链一个
# .a 就够,不像 OpenSSL 那样要一整套 stage。账户表读写两人共用 parlzauth.c。
target_link_libraries(login PRIVATE crypt)
add_executable(user   user.c parlzauth.c)
target_link_libraries(user PRIVATE crypt)
add_executable(sh     sh.c)
add_executable(shx    shx.c)
add_executable(chmod  chmod.c)
add_executable(cat    cat.c)
add_executable(mkdir  mkdir.c)
add_executable(rm     rm.c)
add_executable(file   file.c)
add_executable(cp     cp.c)
add_executable(curl   curl.c http_client.c)
add_executable(ifc    ifc.c)
add_executable(ifconfig ifconfig.c)
add_executable(mount  mount.c)
add_executable(umount umount.c)
add_executable(free   free.c)
add_executable(dmesg  dmesg.c)

# --- 磁盘 / 安装工具 ---
# mkfs 独立目标定义 CLI main();install.c 内含整盘 pwrite 版 ext2 写入
# (write_ext2_whole_disk,与 mkfs.c 布局一致), 不依赖 mkfs.c。
# cpfs: 把 initramfs 根拷贝进分区 2 ext2(动态 inode/块分配)。
add_executable(mkfs    mkfs.c)
target_compile_definitions(mkfs PRIVATE MKFS_CLI_MAIN=1)
add_executable(fdisk   fdisk.c)
add_executable(cpfs    cpfs.c)
add_executable(parlz-install install.c)
add_executable(boot    boot.c)
add_executable(mknod   mknod.c)
# --- 文本工具(P3)---
add_executable(grep    grep.c)
add_executable(head    head.c)
add_executable(tail    tail.c)
# --- 补充常用命令(BUG-16:管道/脚本能力)---
add_executable(wc      wc.c)
add_executable(sort    sort.c)
add_executable(sed     sed.c)
add_executable(awk     awk.c)
add_executable(which   which.c)
add_executable(ps      ps.c)
add_executable(df      df.c)
add_executable(ln      ln.c)
add_executable(mv      mv.c)
add_executable(tree    tree.c)
# --- 包管理器四件套(2026-09-30: 移除上游 OPKG, 改成我们自己移植) ---
# pkgcore.c 是共用的容器/落盘/校验原语: ar + ustar/GNU/pax tar + newc cpio
# + gzip(miniz tinfl) + 安全落盘(拒绝 .. 逃逸、先 unlink 再建、显式 chmod)
# + Debian/RPM 两套版本比较 + SHA-256(核对索引声明的摘要)。
#   dpkg  .deb = ar(debian-binary + control.tar + data.tar), 状态库 /var/lib/dpkg
#   rpm   .rpm = lead + 签名 header + 主 header + cpio 负载, 状态库 /var/lib/rpm
#   apt   高层前端: sources.list + Debian Packages 索引 + 依赖求解, 落盘调 dpkg
#   yum   高层前端: repomd.xml + primary.xml + 依赖求解, 落盘调 rpm
# 都不做 GPG 验签(明说), 取而代之: 未标 trusted/gpgcheck=0 的源一律拒绝安装,
# 下载后按索引声明的 Size + SHA256 核对, 不符就删缓存不交给底层。
add_executable(dpkg  dpkg.c  pkgcore.c miniz_tinfl.c)
add_executable(rpm   rpm.c   pkgcore.c miniz_tinfl.c)
add_executable(apt   apt.c   pkgcore.c miniz_tinfl.c http_client.c)
add_executable(yum   yum.c   pkgcore.c miniz_tinfl.c http_client.c)
# --- 网络工具(ping/wget)---
add_executable(ping    ping.c)
add_executable(wget    wget.c http_client.c)
# --- FRP 客户端(Parlz 简化版 TCP 隧道)---
add_executable(frpc    frpc.c)
# --- P2P VPN 隧道(OpenVPN 最小替代,PVPN1.0)---
add_executable(openvpn openvpn.c)
# --- 音频播放器(自研解码: WAV 直读 + MP3 LameXing 扫描 + FLAC 纯 C 解码;
#     有 /dev/snd 时 raw 出声,无设备打印统计; -s 生成 1KHz 正弦 WAV)---
add_executable(audio audio.c flac_decode.c mp3_decode.c)
target_link_libraries(audio PRIVATE m)
# --- PWeb 轻量 Web 服务器(nginx 风格配置, 静态/CGI/proxy/rewrite)---
# 源码在 userland/pweb/(从仓库根 PWeb/ 移植的 POSIX 版, 纯 C + pthread,
# 零外部依赖; https proxy 默认关闭保持零依赖, 有 OpenSSL 时开启 PWEB_SSL)。
add_executable(pweb
  pweb/main.c pweb/conn.c pweb/config.c pweb/http_parser.c
  pweb/handlers.c pweb/util.c pweb/mime.c pweb/log.c)
target_link_libraries(pweb PRIVATE pthread)
if(PARLZ_HAVE_OPENSSL)
  target_include_directories(pweb PRIVATE ${PARLZ_OPENSSL_PREFIX}/include)
  target_link_libraries(pweb PRIVATE ${PARLZ_OPENSSL_PREFIX}/lib/libssl.a ${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a)
  target_compile_definitions(pweb PRIVATE PWEB_SSL=1)
else()
  target_compile_definitions(pweb PRIVATE PWEB_SSL=0)
endif()
# --- pms.c 邮件客户端(IMAP/SMTP, Win32/Schannel → POSIX socket + 可选
#     OpenSSL; PMS_HAVE_OPENSSL=0 时仅支持明文协议, 静态自包含)---
add_executable(pms pms.c)
if(PARLZ_HAVE_OPENSSL)
  target_include_directories(pms PRIVATE ${PARLZ_OPENSSL_PREFIX}/include)
  target_link_libraries(pms PRIVATE ${PARLZ_OPENSSL_PREFIX}/lib/libssl.a ${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a)
  target_compile_definitions(pms PRIVATE PMS_HAVE_OPENSSL=1)
else()
  target_compile_definitions(pms PRIVATE PMS_HAVE_OPENSSL=0)
endif()
# --- w3m 风格文本浏览器(自研, 复用 http_client 取页 + 简单 CSS 子集)---
add_executable(w3m w3m.c http_client.c)
# --- pm 包管理器(工具链按需安装; newc cpio 包 + http_client feed 下载)---
add_executable(pm pm.c http_client.c)
# PM 自己的版本号(pm+大.小-阶段+第几版): 由 build-userland 从仓库根 .pm-release
# 读出来传进来; 不传则 pm.c 里是 dev 串。
set(PARLZ_PM_VERSION "pm+0.0-A+0-dev" CACHE STRING "PM 发布版本号")
target_compile_definitions(pm PRIVATE PARLZ_PM_VERSION="${PARLZ_PM_VERSION}")
if(PARLZ_HAVE_OPENSSL)
  target_include_directories(w3m PRIVATE ${PARLZ_OPENSSL_PREFIX}/include)
  target_link_libraries(w3m PRIVATE ${PARLZ_OPENSSL_PREFIX}/lib/libssl.a ${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a)
  target_compile_definitions(w3m PRIVATE PARLZ_HAVE_OPENSSL=1)
  # pm: 下载走 http_client(有 OpenSSL 时支持 HTTPS feed, 严格校验)
  target_include_directories(pm PRIVATE ${PARLZ_OPENSSL_PREFIX}/include)
  target_link_libraries(pm PRIVATE ${PARLZ_OPENSSL_PREFIX}/lib/libssl.a ${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a)
  target_compile_definitions(pm PRIVATE PARLZ_HAVE_OPENSSL=1)
  # apt/yum: 同样走 http_client, 没 TLS 就只能配 http:// 的源
  foreach(t apt yum)
    target_include_directories(${t} PRIVATE ${PARLZ_OPENSSL_PREFIX}/include)
    target_link_libraries(${t} PRIVATE ${PARLZ_OPENSSL_PREFIX}/lib/libssl.a ${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a)
    target_compile_definitions(${t} PRIVATE PARLZ_HAVE_OPENSSL=1)
  endforeach()
endif()
# 让 install 命令名也能用
add_custom_command(TARGET parlz-install POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:parlz-install>
          $<TARGET_FILE_DIR:parlz-install>/install)

# --- curl / wget 链接 OpenSSL 静态库(HTTPS)---
if(PARLZ_HAVE_OPENSSL)
  target_include_directories(curl PRIVATE ${PARLZ_OPENSSL_PREFIX}/include)
  target_include_directories(wget PRIVATE ${PARLZ_OPENSSL_PREFIX}/include)
  target_link_libraries(curl PRIVATE ${PARLZ_OPENSSL_PREFIX}/lib/libssl.a ${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a)
  target_link_libraries(wget PRIVATE ${PARLZ_OPENSSL_PREFIX}/lib/libssl.a ${PARLZ_OPENSSL_PREFIX}/lib/libcrypto.a)
  target_compile_definitions(curl PRIVATE PARLZ_HAVE_OPENSSL=1)
  target_compile_definitions(wget PRIVATE PARLZ_HAVE_OPENSSL=1)
endif()

# --- PazeSSH / PazeSSL 命令行工具(pssh 等依赖 PazeSSL,保持不变)---
# pazessl = TLS 客户端/服务器/证书工具; pssh = SSH 客户端。
# 源码在仓库根 TLS-SSH/apps/(单一 main.c),链接 paze.a(由 build-userland.sh 传入)。
set(PAZE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../TLS-SSH)
if(NOT EXISTS "${PAZE_DIR}/include/paze/tls.h")
  set(PAZE_DIR /mnt/f/Linux/Parlz/TLS-SSH)
endif()
if(NOT PAZE_A)
  set(PAZE_A "${CMAKE_CURRENT_SOURCE_DIR}/../../paze.a")
endif()
if(EXISTS "${PAZE_DIR}/include/paze/tls.h" AND EXISTS ${PAZE_A})
  set(PAZESSL_APP ${PAZE_DIR}/apps/pazessl/main.c)
  set(PSSH_APP    ${PAZE_DIR}/apps/pssh/main.c)
  if(EXISTS ${PAZESSL_APP})
    add_executable(pazessl ${PAZESSL_APP})
    target_include_directories(pazessl PRIVATE ${PAZE_DIR}/include)
    target_link_libraries(pazessl PRIVATE ${PAZE_A})
  endif()
  if(EXISTS ${PSSH_APP})
    # pssh 是多命令入口:pssh/main.c 提供 main,各 apps/*/main.c 提供
    # pssh_cmd_xxx(只有 pssh/main.c 定义 main,无冲突)。全部链接 paze.a。
    add_executable(pssh
      ${PSSH_APP}
      ${PAZE_DIR}/apps/ssh/main.c
      ${PAZE_DIR}/apps/keygen/main.c
      ${PAZE_DIR}/apps/keyscan/main.c
      ${PAZE_DIR}/apps/copy_id/main.c
      ${PAZE_DIR}/apps/agent/main.c
      ${PAZE_DIR}/apps/add/main.c
      ${PAZE_DIR}/apps/scp/main.c
      ${PAZE_DIR}/apps/useradd/main.c
      ${PAZE_DIR}/apps/sshd/main.c)
    target_include_directories(pssh PRIVATE ${PAZE_DIR}/include)
    target_link_libraries(pssh PRIVATE ${PAZE_A})
  endif()
endif()
