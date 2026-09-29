const fs = require("fs");
let s = fs.readFileSync("util.c", "latin1");
const nl = String.fromCharCode(10);

// In re_match's kind==3 branch, the balance loop must stop AT the close
// token (hi = index of matching ')'). Restore that.
const old = "        int g = *ng;" + nl +
  "        int hi = ti + 1;" + nl +
  "        int bal = 1;" + nl +
  "        while (hi < rn && bal > 0) {" + nl +
  "            if (rg[hi].kind == 3) { bal++; hi++; }" + nl +
  "            else if (rg[hi].kind == 4) { bal--; }" + nl +
  "            else { hi++; }" + nl +
  "        }";
const neu = "        int g = *ng;" + nl +
  "        int hi = ti + 1;" + nl +
  "        int bal = 1;" + nl +
  "        while (hi < rn && bal > 0) {" + nl +
  "            if (rg[hi].kind == 3) { bal++; hi++; }" + nl +
  "            else if (rg[hi].kind == 4) { bal--; if (bal == 0) { break; } hi++; }" + nl +
  "            else { hi++; }" + nl +
  "        }" + nl +
  "        if (hi >= rn || rg[hi].kind != 4) { return 0; }";
if (s.includes(old)) { s = s.replace(old, neu); console.log("re_match group-close scan fixed"); }
else { console.log("anchor not found in re_match"); }

// Same fix in re_grp_interior's nested-group close scan
const old2 = "            int chi = i + 1;" + nl +
  "            int bal = 1;" + nl +
  "            while (chi < rn && bal > 0) {" + nl +
  "                if (rg[chi].kind == 3) { bal++; }" + nl +
  "                else if (rg[chi].kind == 4) { bal--; chi++; }" + nl +
  "            }" + nl +
  "            if (chi >= rn) { return 0; }";
const neu2 = "            int chi = i + 1;" + nl +
  "            int bal = 1;" + nl +
  "            while (chi < rn && bal > 0) {" + nl +
  "                if (rg[chi].kind == 3) { bal++; chi++; }" + nl +
  "                else if (rg[chi].kind == 4) { bal--; if (bal == 0) { break; } chi++; }" + nl +
  "                else { chi++; }" + nl +
  "            }" + nl +
  "            if (chi >= rn || rg[chi].kind != 4) { return 0; }";
if (s.includes(old2)) { s = s.replace(old2, neu2); console.log("re_grp_interior nested close scan fixed"); }
else { console.log("re_grp_interior anchor not found"); }

fs.writeFileSync("util.c", s, "latin1");
