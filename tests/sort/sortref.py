#!/usr/bin/env python3
"""Reference for SORT.COM: gen N FILE writes synthetic records (no book
data); sort DECK IN OUT sorts IN the way the deck says, stable, fields
past a record's end reading as spaces. Records are lines; output lines
carry no trailing spaces."""
import sys, random, re
def keys(deck):
    spec = ''.join(l.split('FIELDS=(',1)[1] if 'FIELDS=(' in l else l
                   for l in open(deck) if not l.startswith('*'))
    return spec  # unused
def parse(deck):
    text = ''.join(l for l in open(deck) if not l.startswith('*'))
    m = re.search(r'FIELDS=\(([^)]*)\)', text, re.S)
    toks = [t for t in re.sub(r'\s', '', m.group(1)).split(',') if t]
    return [(int(toks[i]), int(toks[i+1]), toks[i+3], toks[i+2]) for i in range(0, len(toks), 4)]
def field(rec, pos, ln):
    return (rec + ' ' * 300)[pos-1:pos-1+ln]
def cmpkey(fields):
    import functools
    def cmp(a, b):
        for pos, ln, order, fmt in fields:
            fa, fb = field(a, pos, ln), field(b, pos, ln)
            if fmt == 'CI': fa, fb = fa.upper(), fb.upper()
            if fa != fb:
                r = -1 if fa < fb else 1
                return -r if order == 'D' else r
        return 0
    return functools.cmp_to_key(cmp)
if sys.argv[1] == 'gen':
    n = int(sys.argv[2]); random.seed(n)
    words = ['ALPHA','BETA','GAMMA','DELTA','OMEGA','ZETA','IOTA','KAPPA']
    with open(sys.argv[3], 'w') as f:
        for i in range(n):
            co = random.randint(1, 3); acct = random.randint(1, 40) * 100
            f.write('%05d%02d%010d%s%s%02d%s%s %d\n' % (
                i+1, co, acct, random.choice('CD'), random.choice('AELQI'),
                random.randint(10, 70), random.choice('YYYN'),
                random.choice(words), random.randint(1, 99)))
else:
    fields = parse(sys.argv[2])
    recs = [l.rstrip('\r\n') for l in open(sys.argv[3])]
    recs.sort(key=cmpkey(fields))
    with open(sys.argv[4], 'w') as f:
        for r in recs: f.write(r.rstrip(' ') + '\n')
