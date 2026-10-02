#!/usr/bin/env python3
# 校验 userland/pkgcore.c 里内嵌的 SHA-256 常量表(K 与 IV)。
# K[i] = floor(frac(cbrt(p_i)) * 2**32), IV[i] = floor(frac(sqrt(p_i)) * 2**32),
# p_i 是第 i 个素数 —— 自己算一遍, 不靠抄表。
import re
import sys


def k_const(p):
    # frac(cbrt(p))*2^32 = floor(cbrt(p*2^96))  (对非完全立方数的 p 成立)
    return icbrt(p << 96) & 0xFFFFFFFF


def icbrt(n):
    lo, hi = 0, 1 << ((n.bit_length() + 2) // 3)
    while lo < hi:
        mid = (lo + hi) // 2
        if mid ** 3 <= n:
            lo = mid + 1
        else:
            hi = mid
    return lo - 1


def iv_const(p):
    return isqrt_fp(p)


def isqrt_fp(p):
    # frac(sqrt(p))*2^32 = floor(sqrt(p*2^64)) 对素数 p 成立
    n = p << 64
    lo, hi = 0, 1 << ((n.bit_length() + 1) // 2)
    while lo < hi:
        mid = (lo + hi) // 2
        if mid * mid <= n:
            lo = mid + 1
        else:
            hi = mid
    return (lo - 1) & 0xFFFFFFFF


def primes(count):
    out = []
    n = 2
    while len(out) < count:
        isp = True
        d = 2
        while d * d <= n:
            if n % d == 0:
                isp = False
                break
            d += 1
        if isp:
            out.append(n)
        n += 1
    return out


def main(path):
    src = open(path, encoding="utf-8").read()
    body = src.split("K256[64] = {")[1].split("};")[0]
    got = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})u?", body)]
    want = [k_const(p) for p in primes(64)]
    bad = [(i, hex(got[i]), hex(want[i])) for i in range(64) if got[i] != want[i]]
    print("K 表: %d 项, 不符 %d 处" % (len(got), len(bad)))
    for b in bad[:10]:
        print("   K[%d] 代码里 %s, 应为 %s" % b)
    m = re.search(r"iv0 = 0x([0-9a-f]+)u.*?iv7 = 0x([0-9a-f]+)u", src, re.S)
    iv = [int(x, 16) for x in m.groups()] if m else []
    want_iv = [iv_const(p) for p in primes(8)]
    print("IV 首尾: %s %s / 应为 %s %s" %
          (hex(iv[0]) if iv else "?", hex(iv[-1]) if iv else "?",
           hex(want_iv[0]), hex(want_iv[-1])))
    ok = not bad and iv and iv[0] == want_iv[0] and iv[-1] == want_iv[-1]
    print("RESULT:", "常量表全部正确" if ok else "常量表有错")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else
                  "userland/pkgcore.c"))
