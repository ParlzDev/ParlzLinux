const fs = require("fs");
// handlers.c -> _port_handlers.c: strip POSIX-only includes and bridge socket ops
let s = fs.readFileSync("handlers.c", "latin1");
// The _port version keeps the same source but with PWEB_* bridges applied to
// bare socket/close calls. Since handlers.c already uses PWEB_* in most places
// (from earlier porting), just copy it through; bridge any remaining bare calls.
s = s.replace(/\brecv\(/g, "PWEB_RECV(");
s = s.replace(/\bsend\(/g, "PWEB_SEND(");
s = s.replace(/\bconnect\(/g, "PWEB_CONNECT(");
s = s.replace(/\bsocket\(/g, "PWEB_SOCKET(");
// Wrap POSIX-only headers so the Windows build (pweb.h provides winsock)
// does not fail on missing <sys/socket.h>/<netinet/in.h>/<netdb.h>.
s = s.replace(
  '#include <sys/socket.h>\n#include <netinet/in.h>\n#include <netdb.h>',
  '#ifndef PWEB_PLATFORM_WIN\n#include <sys/socket.h>\n#include <netinet/in.h>\n#include <netdb.h>\n#endif'
);
fs.writeFileSync("_port_handlers.c", s, "latin1");
const t = fs.readFileSync("_port_handlers.c", "latin1");
let o = 0, c = 0;
for (const ch of t) { if (ch === "{") o++; if (ch === "}") c++; }
console.log("_port_handlers.c regenerated, braces:", o, c, "diff", o - c);
