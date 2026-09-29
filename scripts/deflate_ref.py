# 纯 Python 参考实现: opkg 同款位序 + 逐长度解码
import sys

class BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.reg = 0
        self.bits = 0
    def read(self, n):
        while self.bits < n:
            if self.pos >= len(self.data):
                return 0
            self.reg |= self.data[self.pos] << self.bits
            self.pos += 1
            self.bits += 8
        v = self.reg & ((1 << n) - 1)
        self.reg >>= n
        self.bits -= n
        return v

def build_table(lens, nlens):
    hcount = [0]*32
    for i in range(nlens):
        hcount[lens[i]] += 1
    if hcount[0] == nlens:
        return None
    hcount[1] = 0
    start = [0]*16
    code = 0
    for L in range(1, 16):
        code = (code + hcount[L-1]) << 1
        start[L] = code
    nxt = [0]*nlens
    for i in range(nlens):
        L = lens[i]
        if L and L <= 15:
            raw = start[L]
            start[L] = raw + 1
            rev = 0
            for k in range(L):
                rev = (rev << 1) | ((raw >> k) & 1)
            nxt[i] = rev
    return nxt

def huff_decode(br, nxt, lens, nlens, maxlen):
    for L in range(maxlen, 0, -1):
        raw = br.read(L)
        rev = 0
        for k in range(L):
            rev = (rev << 1) | ((raw >> k) & 1)
        for i in range(nlens):
            if lens[i] == L and nxt[i] == rev:
                return i
        br.bits += L
    return -1

LEN_BASE = [3,4,5,6,7,8,9,10,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258]
LEN_EXTRA = [0,0,0,0,0,0,0,0,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,6]
DIST_BASE = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577,32769,49153]
DIST_EXTRA = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14]

def inflate(d):
    br = BitReader(d)
    out = bytearray()
    while True:
        bfinal = br.read(1)
        btype = br.read(2)
        if btype == 0:
            br.bits = 0
            br.pos = (br.pos + 7) // 8
            ln = d[br.pos] | (d[br.pos+1] << 8)
            nln = d[br.pos+2] | (d[br.pos+3] << 8)
            br.pos += 4
            assert ln == (~nln & 0xffff), "stored NLEN 不匹配"
            out += d[br.pos:br.pos+ln]
            br.pos += ln
            if bfinal:
                break
            continue
        lens = [0]*288
        dlens = [0]*32
        if btype == 1:
            for i in range(144): lens[i] = 8
            for i in range(144,256): lens[i] = 9
            for i in range(256,280): lens[i] = 7
            for i in range(280,288): lens[i] = 8
            for i in range(32): dlens[i] = 5
            hlit = 288; hdist = 32
            nxt = build_table(lens, 288)
            dnext = build_table(dlens, 32)
        else:
            hlit = br.read(5) + 257
            hdist = br.read(5) + 1
            hlen = br.read(4) + 4
            order = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15]
            clens = [0]*19
            for i in range(hlen):
                clens[order[i]] = br.read(3)
            clnxt = build_table(clens, 19)
            seq = []
            for i in range(hlit + hdist):
                s = huff_decode(br, clnxt, clens, 19, 7)
                if s < 0:
                    raise SystemExit("码长符号 FAIL at %d" % i)
                if s < 16:
                    seq.append(s)
                elif s == 16:
                    rep = 3 + br.read(2)
                    seq.extend([seq[-1]]*rep)
                elif s == 17:
                    rep = 3 + br.read(3)
                    seq.extend([0]*rep)
                else:
                    rep = 11 + br.read(7)
                    seq.extend([0]*rep)
            for i in range(hlit):
                lens[i] = seq[i]
            for i in range(hdist):
                dlens[i] = seq[hlit+i]
            nxt = build_table(lens, 288)
            dnext = build_table(dlens, 32)
        while True:
            s = huff_decode(br, nxt, lens, 288, 15)
            if s < 0:
                raise SystemExit("lit/len FAIL out=%d" % len(out))
            if s < 256:
                out.append(s)
            elif s == 256:
                break
            else:
                li = s - 257
                length = LEN_BASE[li] + br.read(LEN_EXTRA[li])
                di = huff_decode(br, dnext, dlens, 32, 15)
                dist = DIST_BASE[di] + br.read(DIST_EXTRA[di])
                for k in range(length):
                    out.append(out[-dist])
        if bfinal:
            break
    return bytes(out)

if __name__ == "__main__":
    g = open(sys.argv[1], "rb").read()
    assert g[0] == 0x1f and g[1] == 0x8b, "非 gzip"
    off = 10
    flg = g[3]
    if flg & 4:
        xl = g[off] | (g[off+1] << 8)
        off += 2 + xl
    if flg & 8:
        while g[off]: off += 1
        off += 1
    if flg & 16:
        while g[off]: off += 1
        off += 1
    data = inflate(g[off:len(g)-8])
    import gzip
    ref = gzip.decompress(g)
    print("ref  %d B  %s" % (len(ref), "OK" if ref else "EMPTY"))
    print("mine %d B  一致: %s" % (len(data), data == ref))
    if data != ref:
        for i in range(min(len(data), len(ref))):
            if data[i] != ref[i]:
                print("首差异 @%d: mine=%02x ref=%02x" % (i, data[i], ref[i]))
                break
