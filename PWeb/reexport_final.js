const fs = require("fs");
// conn -> _port_conn
let s = fs.readFileSync("conn.c", "latin1");
s = s.replace("#include <sys/socket.h>\n", "");
s = s.replace("#include <netinet/in.h>\n", "");
s = s.replace("#include <netdb.h>\n", "");
s = s.replace("#include <pthread.h>\n", "");
s = s.replace(/\bPWEB_RECV\(/g, "PWEB_RECV(");
s = s.replace(/\bPWEB_SEND\(/g, "PWEB_SEND(");
s = s.replace(/\bPWEB_CLOSE\(fd\)/g, "PWEB_CLOSE(fd)");
fs.writeFileSync("_port_conn.c", s, "latin1");

// main -> _port_main
s = fs.readFileSync("main.c", "latin1");
s = s.replace("#include <sys/socket.h>\n", "");
s = s.replace("#include <netinet/in.h>\n", "");
s = s.replace("#include <arpa/inet.h>\n", "");
s = s.replace("#include <signal.h>\n", "");
s = s.replace("#include <unistd.h>\n", "");
s = s.replace(/pthread_t th;/g, "void *th;");
fs.writeFileSync("_port_main.c", s, "latin1");

// config -> _port_config (pure POSIX, no socket ops)
s = fs.readFileSync("config.c", "latin1");
fs.writeFileSync("_port_config.c", s, "latin1");

// util -> _port_util
s = fs.readFileSync("util.c", "latin1");
fs.writeFileSync("_port_util.c", s, "latin1");

console.log("regenerated _port_conn/_port_main/_port_config/_port_util");
for (const f of ["_port_conn.c", "_port_main.c", "_port_config.c", "_port_util.c"]) {
  const t = fs.readFileSync(f, "latin1");
  let o = 0, c = 0;
  for (const ch of t) { if (ch === "{") o++; if (ch === "}") c++; }
  console.log(f, "braces:", o, c, "diff", o - c);
}
