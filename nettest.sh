#!/bin/sh
echo "=== TEST: bash 特性 ==="
bash -c 'arr=(a "b c" d); echo ARR=${arr[1]}'
bash -c 'echo BRACE={1..5}'
bash -c 'echo -e "E1\nE2"'
bash -c '[[ abc == a* ]]; echo GLOBQ=$?'
bash -c 'p(){ /bin/cat <(echo PROC_OK); }; p'
echo "=== TEST: chmod ==="
echo perm_test > /tmp/perm.txt
chmod -v 000 /tmp/perm.txt
cat /tmp/perm.txt && echo ROOT_READ_OK
chmod 644 /tmp/perm.txt
cat /tmp/perm.txt && echo CHMOD_RESTORE_OK
echo "=== TEST: grep 文件参数(修复验证) ==="
printf 'alpha one\nbeta two\nalpha three\n' > /tmp/gt.txt
grep alpha /tmp/gt.txt > /tmp/grep_out.txt
N=$(wc -l < /tmp/grep_out.txt)
[ "$N" -eq 2 ] && echo GREP_FILE_OK || echo "GREP_FILE_FAIL N=$N"
echo "=== TEST: opkg/ppm 委托 ==="
opkg --version
ppm opkg --version
echo "=== TEST: curl HTTP(host 10.0.2.2:8080) ==="
curl -s -m 10 -o /tmp/h.txt http://10.0.2.2:8080/hello.txt \
  && cat /tmp/h.txt && echo HTTP_OK
echo "=== TEST: curl 严格 HTTPS 无 CA 文件、系统束验证宿主 CA(应通过) ==="
curl -s -m 10 -o /tmp/ss.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/ss.txt && echo STRICT_SYS_OK
echo "=== TEST: curl 严格 HTTPS + --cacert(应通过) ==="
curl -s -m 10 --cacert /probe-ca.pem -o /tmp/hs.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/hs.txt && echo STRICT_CACERT_OK
echo "=== TEST: curl 严格 HTTPS + 假 CA(应失败关闭) ==="
printf 'not a certificate\n' > /tmp/badca.pem
curl -s -m 10 --cacert /tmp/badca.pem -o /dev/null https://10.0.2.2:8443/hello.txt \
  && echo STRICT_BADCA_UNEXPECTED \
  || echo STRICT_BADCA_REJECT
echo "=== TEST: curl --insecure TLS(对照) ==="
curl -s -m 10 --insecure -o /tmp/i.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/i.txt && echo INSECURE_OK
echo "=== TEST: wget HTTPS --ca-certificate=等号形式 ==="
wget -q --ca-certificate=/probe-ca.pem -O /tmp/ws.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/ws.txt && echo WGET_CACERT_OK
echo "=== TEST: wget HTTP ==="
wget -q -O /tmp/w.txt http://10.0.2.2:8080/hello.txt \
  && cat /tmp/w.txt && echo WGET_OK
echo "=== 全部测试完成 ==="
echo E2E_FINISHED
