; bootcode.nas - Parlz MBR 引导代码(NASM 16-bit 实模式)
;
; 构建: nasm -f bin bootcode.nas -o /tmp/mbr.bin
;       scripts/build-userland.sh 用 objcopy 把 mbr.bin 转成 C 数组
;       bootcode.h(mbr_bootcode[446]),install.c #include
;
; 功能:BIOS 把 MBR 加载到 0x7C00。
;   1. 清段寄存器组,设栈(0x7E00,避开 0x07C0 目标区)
;   2. INT 13h AH=42(LBA 模式):从 LBA 16384(vmlinuz setup 头)
;      读 1 扇区到 0x07C0(标准 boot sector 地址)
;   3. jmp far 0x0000:0x07C0 跳进 vmlinuz setup 头;
;      setup 头按 boot sector 协议自行读 LBA 16385 起的剩余
;      vmlinuz,解压到 0x100000,进入 64 位内核。
;
; 约束:总长必须 < 446 字节(后 2 字节留给 0x55AA,由安装器写)

org 0x7C00

; ---- 头部:跳转 + 文字 ----
    jmp  short .code
    db "PARLZ MBR"
    times 65 db 0x90            ; NOP pad 到偏移 0x48

.code:
    ; 清段寄存器组。栈 0x7E00(低区)。MBR 在 0x7C00。
    ; INT 13h AH=42(LBA 模式)读 LBA 16384(vmlinuz setup 头)1 扇区到
    ; 0x07C0(标准 boot sector 地址),跳 0x07C0:0。
    ; 必须 0x07C0:setup 头的 setup_sig/HdrS 段布局假设 boot sector
    ; 加载在 0x07C0,搬到 0x1000 段会偏移错乱导致 setup 失败。
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7E00

    ; ---- LBA 读 LBA 16384(1 扇区)到 0x07C0 ----
    mov  ah, 0x42              ; EXTENDED READ
    mov  al, 0x01              ; 1 扇区
    mov  cx, 0
    mov  dx, 0x4000            ; LBA 16384(低 16 位)
    mov  ax, 0x0000            ; ES=0x0000(0x07C0 在低段)
    mov  es, ax
    mov  bx, 0x07C0
    cli
    int  0x13
    popf
    jc  .retry
    jmp  .ok

.retry:
    ; 复位重试 3 次
    mov  cx, 3
.rtry2:
    mov  ah, 0x00
    cli
    int  0x13
    popf
    mov  ah, 0x42
    mov  al, 0x01
    mov  cx, 0
    mov  dx, 0x4000
    mov  ax, 0x0000
    mov  es, ax
    mov  bx, 0x07C0
    cli
    int  0x13
    popf
    jnc  .ok
    dec  cx
    jnz  .rtry2
    jmp  .dead

.ok:
    jmp  far 0x0000:0x07C0     ; 跳进 vmlinuz setup 头(0x07C0)

.dead:
    jmp  .dead

; 尾部:代码到此结束。生成 bootcode.h 时由 C 侧补 0 到 446 字节,
; 最后 2 字节 0x55AA 由安装器 write_mbr 写。
