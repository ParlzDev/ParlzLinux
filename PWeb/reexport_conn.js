const fs = require("fs");
// conn.c -> _port_conn.c : drop POSIX-only headers, bridge socket ops.
let s = fs.readFileSync("conn.c", "latin1");
s = s.replace("#include <sys/socket.h>\n", "");
s = s.replace("#include <netinet/in.h>\n", "");
s = s.replace("#include <netdb.h>\n", "");
s = s.replace("#include <pthread.h>\n", "");
// bare socket calls -> PWEB_* bridges (MinGW Winsock)
s = s.replace(/\brecv\(/g, "PWEB_RECV(");
s = s.replace(/\bsend\(/g, "PWEB_SEND(");
// inet_ntop is not in Winsock; bridge to PWEB_INET_NTOP (see pweb.h)
s = s.replace(/\binet_ntop\(/g, "PWEB_INET_NTOP(");
fs.writeFileSync("_port_conn.c", s, "latin1");
console.log("regenerated _port_conn.c");
const t = fs.readFileSync("_port_conn.c", "latin1");
let o = 0, c = 0;
for (const ch of t) { if (ch === "{") o++; if (ch === "}") c++; }
console.log("_port_conn.c braces:", o, c, "diff", o - c);
