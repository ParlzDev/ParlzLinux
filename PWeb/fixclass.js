const fs = require("fs");
let s = fs.readFileSync("util.c", "latin1");
const nl = String.fromCharCode(10);

// Bug: for kind==2 (class), re_parse_class already consumed the class AND the
// quantifier can't be attached because the `if (t.kind >= 0 && t.kind <= 2)`
// block runs AFTER p has been advanced by re_parse_class — that's fine.
// The real bug: re_parse_class's negated-class XOR sets cls[0]=0 which is right,
// but the class itself is parsed correctly. Let me verify by checking [^a] on "b".
// Actually test 18 ([^a] vs b) failing means the class bytes are wrong.
// Check re_parse_class: for "[^a]", p points at '['. neg=1, p->'^'->'a'.
// loop: c='a', p->']'. lo=hi='a'. cls['a']=1. Then p=']' consumed. neg: XOR all.
// That should give cls['b']=1. So [^a] should match b. Unless the compile loop
// mis-parses because re_parse_class takes &p but p is a local `const char*`...
// re_parse_class(&p, &t) — p is the local pointer, good.

// The likely real issue: test 19 "a(b)1" — backreference not supported (fine,
// mark unsupported). Test 20-22 involve (.+) / groups with quantifiers where
// re_grp_interior's greedy path returns but the OUTER re_match group-open
// backtracking doesn't propagate the final position correctly.

// Fix: in re_match's kind==3 branch, the inner re_grp_interior call must be
// able to FAIL and let the group start advance. Currently it does. But the bug
// is that after re_grp_interior succeeds it returns `gp` (just past interior),
// and re_match continues at hi+1 with `gp`. That's correct.

// Let me just test [^a] in isolation to isolate the class bug.
