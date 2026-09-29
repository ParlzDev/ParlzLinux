; cdboot.nas - 可引导 ISO 引导扇区(SeaBIOS 把 512B boot image 加载到
; 20:0,即物理 0x200;INT 13h AH=42 LBA 读相对 20:0 无歧义)。
; 构建: sh scripts/gen-cdboot.sh LBA
;
; 职责: 一次性把整块 vmlinuz(内嵌 rootfs 的 bzImage,约 29000 扇区)
; 从 LBA CDSLBA 读到 0x20000(段 0x2000,避开 MBR 0x7C00 与 BDA 区),
; 再把 setup 头(前 512B)复制到 0x7C0:0,跳 0x7C0:0。
; setup 头按 boot sector 协议自读 0x7C0+1 起的剩余镜像(全部已在
; 低内存 0x20000..),解压到 0x100000 进 64 位内核。
; 这样彻底绕开 setup 头 root 字段与磁盘几何,不依赖 INT 13h 二次读。
;
; 内存预算: 0x20000 + 29154*512 ≈ 0x20000+0x3C200 = 0x5C200 (< 363KB,
; 常规 640KB conventional 内存内)。栈 0x7E00。
org 0

    jmp  short .code
    db "PARLZ CDBOOT"
    times 26 db 0x90            ; NOP pad 到 0x28

.code:                          ; 0x28
    xor  ax, ax
    mov  ds, ax                ; DS=ES=0(段基址 0,数据从 0x20000 写)
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7E00

    ; ---- 1) 整块 vmlinuz 读入 0x20000(ES:BX = 0:0x20000)----
    ; 扇区数 = CDSLBA 起 CDTOTAL(低 16 位;LBA<65536 时 cx:dx 全放 cx)
    mov  ah, 0x42
    mov  al, CDTOTAL          ; N 扇区(低字节;N<256? 否!需 32 位 LBA 读)
    ; 注意: AH=42 的扇区数在 AL(低 8 位)不足 29000,用 AH=43 扩展
    ; 48-bit LBA 读(支持大扇区数 + LBA>0xFFFF)。AH=43:
    ;   DL=盘, ESI=目标扇区数, CH:LBA低24, CL, DH, DI:LBA24..39
    ; QEMU virtio-blk 扇区数 ~29154, 用 43h(48位LBA)最稳。
    jmp  .read48
.read48:
    mov  ah, 0x43              ; EXTENDED READ (48-bit LBA)
    mov  dl, 0                 ; 当前活动磁盘
    mov  esi, CDTOTAL          ; 总扇区数(16 位足够, < 65536)
    ; LBA = CDSLBA(低 24 位放 CH/CL/DH, 高 24 位放 DI)
    mov  ch, CDHDL             ; LBA 高 8 位(bit8-15)
    mov  cl, CDLDL             ; LBA 低 8 位(bit0-6)
    mov  dh, 0                 ; bit16-23 在 DH; 0x00(本盘 LBA<65536*256)
    mov  di, CDHHD             ; bit24-39
    mov  ax, 0x2000
    mov  es, ax                ; 目标 0x20000
    mov  bx, 0
    cli
    int  0x13
    popf
    jc  .retry
    jmp  .ok

.retry:
    mov  cx, 3
.r2:
    mov  ah, 0x00
    cli
    int  0x13
    popf
    mov  ah, 0x43
    mov  dl, 0
    mov  esi, CDTOTAL
    mov  ch, CDHDL
    mov  cl, CDLDL
    mov  dh, 0
    mov  di, CDHHD
    mov  ax, 0x2000
    mov  es, ax
    mov  bx, 0
    cli
    int  0x13
    popf
    jnc  .ok
    dec  cx
    jnz  .r2
    jmp  .dead

.ok:
    ; ---- 2) 前 512B(= setup 头)从 0x20000 复制到 0x7C0:0 ----
    mov  ax, 0
    mov  ds, ax
    mov  es, ax
    mov  si, 0x2000            ; 0:0x20000
    mov  di, 0x07C0            ; 0:0x07C0
    mov  cx, 0x200             ; 512B
    cld
    rep  movsb

    ; ---- 3) 跳 setup 头(boot sector 协议地址 0x7C0:0)----
    jmp  far 0x0000:0x07C0

.dead:
    jmp  .dead

; 尾部填充由 gen-cdboot.sh pad 到 512。
; 占位符: CDTOTAL(扇区数), CDHDL/CDLDL/CDHHD(LBA 拆分)。
