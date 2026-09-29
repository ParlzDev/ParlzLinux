const fs = require("fs");
let s = fs.readFileSync("util.c", "latin1");
const nl = String.fromCharCode(10);
const oldBlock = "    if (neg) {" + nl +
  "        for (int i = 0; i < 128; i++) { t->cls[i] ^= 1; }" + nl +
  "        t->cls[0] = 0;" + nl +
  "    }";
const newBlock = "    if (neg) {" + nl +
  "        for (int i = 1; i < 128; i++) { t->cls[i] = !t->cls[i]; }" + nl +
  "        t->cls[0] = 0;" + nl +
  "    }";
if (s.includes(oldBlock)) { s = s.replace(oldBlock, newBlock); console.log("class negation fixed"); }
else {
  const lines = s.split(nl);
  let i = lines.findIndex(l => l.includes("for (int i = 0; i < 128; i++) { t->cls[i] ^= 1; }"));
  if (i >= 0) { lines[i] = "        for (int i = 1; i < 128; i++) { t->cls[i] = !t->cls[i]; }"; s = lines.join(nl); console.log("patched class negation at line", i + 1); }
  else console.log("class negation block not found");
}
fs.writeFileSync("util.c", s, "latin1");
