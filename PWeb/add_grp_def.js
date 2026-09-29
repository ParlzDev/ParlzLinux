const fs = require("fs");
let lines = fs.readFileSync("util.c", "latin1").split(String.fromCharCode(10));
// Insert the re_grp_interior definition just before re_match's definition
let i = lines.findIndex(l => l.startsWith("static int re_match("));
// The forward decl + its trailing blank was inserted before re_match. Insert the
// full definition right after the forward decl (i.e., at i, which now points to
// the re_match def line because we want the real def before re_match too).
// Place it BEFORE the forward decl is fine since it's a definition; put it
// right before re_match definition.
const def = [
"/* Match a group interior [lo,hi) (hi = matching close token index) starting",
" * at `pos`. On success, *outp points just past the matched content.",
" * Nested groups are handled by recursing into re_grp_interior itself; the",
" * outer re_match drives top-level matching. */",
"static int re_grp_interior(int lo, int hi, const char *pos, int depth,",
"                           int *capst, int *caplen, int *ng,",
"                           const char *str_start, const char *str_end,",
"                           const char **outp)",
"{",
"    if (depth > 4096) { return 0; }",
"    const char *cur = pos;",
"    for (int i = lo; i < hi; i++) {",
"        const retok *t = &rg[i];",
"        if (t->kind == 3) {",
"            int g = *ng;",
"            int chi = i + 1;",
"            int bal = 1;",
"            while (chi < rn && bal > 0) {",
"                if (rg[chi].kind == 3) { bal++; }",
"                else if (rg[chi].kind == 4) { bal--; }",
"                chi++;",
"            }",
"            if (chi > hi) { return 0; }",
"            const char *gstart = cur;",
"            int ok = 0;",
"            for (;;) {",
"                if (g < PWEB_RE_MAXCAPS) { capst[g] = (int)(gstart - str_start); }",
"                const char *gp = gstart;",
"                int ng_i = *ng;",
"                if (re_grp_interior(i + 1, chi, gstart, depth + 1,",
"                                    capst, caplen, &ng_i,",
"                                    str_start, str_end, &gp)) {",
"                    if (g < PWEB_RE_MAXCAPS) { caplen[g] = (int)(gp - gstart); }",
"                    *ng = g + 1;",
"                    cur = gp;",
"                    ok = 1;",
"                    i = chi;",
"                    break;",
"                }",
"                gstart++;",
"                if (gstart > str_end) { break; }",
"            }",
"            if (!ok) {",
"                if (g < PWEB_RE_MAXCAPS) { capst[g] = -1; caplen[g] = 0; }",
"                return 0;",
"            }",
"            continue;",
"        }",
"        if (t->kind == 5) { if (cur != str_start) { return 0; } continue; }",
"        if (t->kind == 6) { if (cur != str_end) { return 0; } continue; }",
"        if (t->qtype == 0) {",
"            if (!re_one(t, cur)) { return 0; }",
"            cur++;",
"        } else if (t->qtype == 3) {",
"            if (re_one(t, cur)) { cur++; }",
"        } else {",
"            int lo2 = (t->qtype == 4) ? t->qn : (t->qtype == 2 ? 1 : 0);",
"            int hi2 = (t->qtype == 4) ? t->qm : 999;",
"            int cnt = 0;",
"            const char *p2 = cur;",
"            while (cnt < hi2 && re_one(t, p2)) { p2++; cnt++; }",
"            if (cnt < lo2) { return 0; }",
"            cur = p2; /* greedy max run */",
"        }",
"    }",
"    *outp = cur;",
"    return 1;",
"}",
"",
];
lines.splice(i, 0, ...def);
fs.writeFileSync("util.c", lines.join(String.fromCharCode(10)), "latin1");
console.log("re_grp_interior definition inserted");
let t = fs.readFileSync("util.c", "latin1");
let o = 0, c = 0;
for (const ch of t) { if (ch === "{") o++; if (ch === "}") c++; }
console.log("braces:", o, c, "diff", o - c);
