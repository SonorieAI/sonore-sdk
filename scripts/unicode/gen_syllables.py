# SPDX-License-Identifier: Apache-2.0
"""Writes the syllable machines of gfx/text_shaper.h: one DFA per shaper --
Indic, Khmer, Myanmar, Universal -- from HarfBuzz's grammars (MIT; see
THIRD_PARTY_NOTICES), as Ragel builds them for HarfBuzz: at each position the
longest match of any pattern, the earliest pattern on a tie.

    python gen_syllables.py    (regen.py runs it)

The grammars below are HarfBuzz's hb-ot-shaper-*-machine.rl, the expressions
as written there. Each machine is minimised and printed as a transition
table over the categories it names, an "accepts" table -- the syllable type
a state ends, 0xFF for none -- and the category-to-column map.
"""
import re

INDIC = dict(
    exports=dict(X=0, C=1, V=2, N=3, H=4, ZWNJ=5, ZWJ=6, M=7, SM=8, A=9, VD=9, PLACEHOLDER=10,
                 DOTTEDCIRCLE=11, RS=12, MPst=13, Repha=14, Ra=15, CM=16, Symbol=17, CS=18, SMPst=57),
    rules='''
c = (C | Ra);
n = ((ZWNJ?.RS)? (N.N?)?);
z = ZWJ|ZWNJ;
reph = (Ra H | Repha);
sm = SM | SMPst;
cn = c.ZWJ?.n?;
symbol = Symbol.N?;
matra_group = z*.(M | sm? MPst).N?.H?;
syllable_tail = (z?.sm.sm?.ZWNJ?)? (A | VD)*;
halant_group = (z?.H.(ZWJ.N?)?);
final_halant_group = halant_group | H.ZWNJ;
medial_group = CM?;
halant_or_matra_group = (final_halant_group | matra_group*);
complex_syllable_tail = (halant_group.cn)* medial_group halant_or_matra_group syllable_tail;
consonant_syllable = (Repha|CS)? cn complex_syllable_tail;
vowel_syllable = reph? V.n? (ZWJ | complex_syllable_tail);
standalone_cluster = ((Repha|CS)? PLACEHOLDER | reph? DOTTEDCIRCLE).n? complex_syllable_tail;
symbol_cluster = symbol syllable_tail;
broken_cluster = reph? n? complex_syllable_tail;
other = any;
''',
    main=[('consonant_syllable', 0), ('vowel_syllable', 1), ('standalone_cluster', 2),
          ('symbol_cluster', 3), ('SMPst', 5), ('broken_cluster', 4), ('other', 5)])

KHMER = dict(
    exports=dict(C=1, V=2, H=4, ZWNJ=5, ZWJ=6, PLACEHOLDER=10, DOTTEDCIRCLE=11, Ra=15, VAbv=20,
                 VBlw=21, VPre=22, VPst=23, Robatic=25, Xgroup=26, Ygroup=27),
    rules='''
c = (C | Ra | V);
cn = c.((ZWJ|ZWNJ)?.Robatic)?;
joiner = (ZWJ | ZWNJ);
xgroup = (joiner*.Xgroup)*;
ygroup = Ygroup*;
matra_group = VPre? xgroup VBlw? xgroup (joiner?.VAbv)? xgroup VPst?;
syllable_tail = xgroup matra_group xgroup (H.c)? ygroup;
broken_cluster = Robatic? (H.cn)* (H | syllable_tail);
consonant_syllable = (cn|PLACEHOLDER|DOTTEDCIRCLE) broken_cluster;
other = any;
''',
    main=[('consonant_syllable', 0), ('broken_cluster', 1), ('other', 2)])

MYANMAR = dict(
    exports=dict(C=1, IV=2, DB=3, H=4, ZWNJ=5, ZWJ=6, SM=8, GB=10, DOTTEDCIRCLE=11, A=9, Ra=15,
                 CS=18, SMPst=57, VAbv=20, VBlw=21, VPre=22, VPst=23, As=32, MH=35, MR=36, MW=37,
                 MY=38, PT=39, VS=40, ML=41),
    rules='''
j = ZWJ|ZWNJ;
k = (Ra As H);
sm = SM | SMPst;
c = C|Ra;
medial_group = MY? As? MR? ((MW MH? ML? | MH ML? | ML) As?)?;
main_vowel_group = (VPre.VS?)* VAbv* VBlw* A* (DB As?)?;
post_vowel_group = VPst MH? ML? As* VAbv* A* (DB As?)?;
tone_group = sm | PT A* DB? As?;
complex_syllable_tail = As* medial_group main_vowel_group post_vowel_group* tone_group* j?;
syllable_tail = (H (c|IV).VS?)* (H | complex_syllable_tail);
consonant_syllable = (k|CS)? (c|IV|GB|DOTTEDCIRCLE).VS? syllable_tail;
broken_cluster = k? VS? syllable_tail;
other = any;
''',
    main=[('consonant_syllable', 0), ('j | SMPst', 2), ('broken_cluster', 1), ('other', 2)])

USE = dict(
    exports=dict(O=0, B=1, N=4, GB=5, CGJ=6, SUB=11, H=12, HN=13, ZWNJ=14, WJ=16, R=18, CS=43, IS=44,
                 Sk=48, G=49, J=50, SB=51, SE=52, HVM=53, HM=54, HR=55, RK=56, FAbv=24, FBlw=25,
                 FPst=26, MAbv=27, MBlw=28, MPst=29, MPre=30, CMAbv=31, CMBlw=32, VAbv=33, VBlw=34,
                 VPst=35, VPre=22, VMAbv=37, VMBlw=38, VMPst=39, VMPre=23, SMAbv=41, SMBlw=42,
                 FMAbv=45, FMBlw=46, FMPst=47),
    rules='''
h = H | HVM | IS | Sk;
consonant_modifiers = CMAbv* CMBlw* ((h B | SUB) CMAbv* CMBlw*)*;
medial_consonants = MPre? MAbv? MBlw? MPst?;
dependent_vowels = VPre* VAbv* VBlw* VPst* | H;
vowel_modifiers = HVM? VMPre* VMAbv* VMBlw* VMPst*;
final_consonants = FAbv* FBlw* FPst*;
final_modifiers = FMAbv* FMBlw* | FMPst?;
complex_syllable_start = (R | CS)? (B | GB);
complex_syllable_middle = consonant_modifiers medial_consonants dependent_vowels vowel_modifiers (Sk B)*;
complex_syllable_tail = complex_syllable_middle final_consonants final_modifiers;
number_joiner_terminated_cluster_tail = (HN N)* HN;
numeral_cluster_tail = (HN N)+;
symbol_cluster_tail = SMAbv+ SMBlw* | SMBlw+;
virama_terminated_cluster_tail = consonant_modifiers (IS | RK);
virama_terminated_cluster = complex_syllable_start virama_terminated_cluster_tail;
sakot_terminated_cluster_tail = complex_syllable_middle Sk;
sakot_terminated_cluster = complex_syllable_start sakot_terminated_cluster_tail;
standard_cluster = complex_syllable_start complex_syllable_tail;
tail = complex_syllable_tail | sakot_terminated_cluster_tail | symbol_cluster_tail | virama_terminated_cluster_tail;
broken_cluster = R? (tail | number_joiner_terminated_cluster_tail | numeral_cluster_tail);
number_joiner_terminated_cluster = N number_joiner_terminated_cluster_tail;
numeral_cluster = N numeral_cluster_tail?;
symbol_cluster = (O | GB | SB) tail?;
hieroglyph_cluster = SB* G HR? HM? SE* (J SB* (G HR? HM? SE*)?)*;
other = any;
''',
    main=[('virama_terminated_cluster ZWNJ?', 0), ('sakot_terminated_cluster ZWNJ?', 1),
          ('standard_cluster ZWNJ?', 2), ('number_joiner_terminated_cluster ZWNJ?', 3),
          ('numeral_cluster ZWNJ?', 4), ('symbol_cluster ZWNJ?', 5), ('hieroglyph_cluster ZWNJ?', 6),
          ('FMPst', 8), ('broken_cluster ZWNJ?', 7), ('other', 8)])


# ── Regular expressions to an NFA ───────────────────────────────────────────

class Nfa:
    def __init__(self):
        self.eps = []    # state -> [state]
        self.moves = []  # state -> [(frozenset of symbols, state)]

    def state(self):
        self.eps.append([])
        self.moves.append([])
        return len(self.eps) - 1


def tokens(text):
    return re.findall(r'[A-Za-z_][A-Za-z_0-9]*|[()|.?*+]', text)


class Parser:
    """expr := term ('|' term)*; term := factor (('.')? factor)*;
    factor := atom ('?' | '*' | '+')*; atom := name | '(' expr ')'."""

    def __init__(self, nfa, defs, symbols, alphabet):
        self.nfa, self.defs, self.symbols, self.alphabet = nfa, defs, symbols, alphabet

    def parse(self, text):
        self.toks, self.pos = tokens(text), 0
        frag = self.expr()
        assert self.pos == len(self.toks), text
        return frag

    def peek(self):
        return self.toks[self.pos] if self.pos < len(self.toks) else None

    def expr(self):
        a = self.term()
        while self.peek() == '|':
            self.pos += 1
            b = self.term()
            s, e = self.nfa.state(), self.nfa.state()
            self.nfa.eps[s] += [a[0], b[0]]
            self.nfa.eps[a[1]].append(e)
            self.nfa.eps[b[1]].append(e)
            a = (s, e)
        return a

    def term(self):
        a = self.factor()
        while True:
            t = self.peek()
            if t == '.':
                self.pos += 1
            elif t is None or t in ('|', ')'):
                return a
            b = self.factor()
            self.nfa.eps[a[1]].append(b[0])
            a = (a[0], b[1])

    def factor(self):
        a = self.atom()
        while self.peek() in ('?', '*', '+'):
            op = self.toks[self.pos]
            self.pos += 1
            s, e = self.nfa.state(), self.nfa.state()
            self.nfa.eps[s].append(a[0])
            self.nfa.eps[a[1]].append(e)
            if op in ('?', '*'):
                self.nfa.eps[s].append(e)
            if op in ('*', '+'):
                self.nfa.eps[a[1]].append(a[0])
            a = (s, e)
        return a

    def atom(self):
        t = self.toks[self.pos]
        self.pos += 1
        if t == '(':
            a = self.expr()
            assert self.toks[self.pos] == ')'
            self.pos += 1
            return a
        if t in self.defs:
            # A definition is expanded where it is used, as Ragel does.
            saved = (self.toks, self.pos)
            frag = self.parse(self.defs[t])
            self.toks, self.pos = saved
            return frag
        s, e = self.nfa.state(), self.nfa.state()
        syms = frozenset(self.alphabet) if t == 'any' else frozenset([self.symbols[t]])
        self.nfa.moves[s].append((syms, e))
        return (s, e)


def build(machine):
    exports = machine['exports']
    values = sorted(set(exports.values()))
    column = {v: k for k, v in enumerate(values)}
    other = len(values)           # every category the grammar does not name
    alphabet = list(range(other + 1))
    symbols = {name: column[v] for name, v in exports.items()}
    defs = {}
    for line in machine['rules'].strip().split(';'):
        line = line.strip()
        if not line:
            continue
        name, body = line.split('=', 1)
        defs[name.strip()] = body.strip()
    nfa = Nfa()
    start = nfa.state()
    accepting = {}
    for prio, (text, kind) in enumerate(machine['main']):
        frag = Parser(nfa, defs, symbols, alphabet).parse(text)
        nfa.eps[start].append(frag[0])
        accepting[frag[1]] = (prio, kind)

    def closure(states):
        stack, seen = list(states), set(states)
        while stack:
            for t in nfa.eps[stack.pop()]:
                if t not in seen:
                    seen.add(t)
                    stack.append(t)
        return frozenset(seen)

    # Subset construction; state 0 is the dead state.
    first = closure([start])
    dfa = {first: 1}
    order = [None, first]
    trans = {0: [0] * len(alphabet)}
    accept = {0: None}
    k = 1
    while k < len(order):
        cur = order[k]
        best = None
        for s in cur:
            if s in accepting and (best is None or accepting[s][0] < best[0]):
                best = accepting[s]
        accept[k] = best[1] if best else None
        row = []
        for a in alphabet:
            nxt = set()
            for s in cur:
                for syms, t in nfa.moves[s]:
                    if a in syms:
                        nxt.add(t)
            if not nxt:
                row.append(0)
                continue
            c = closure(nxt)
            if c not in dfa:
                dfa[c] = len(order)
                order.append(c)
            row.append(dfa[c])
        trans[k] = row
        k += 1

    # Moore's minimisation, the dead state kept apart as state 0.
    n = len(order)
    block = [0] + [1 + sorted(set(accept[s] if accept[s] is not None else -1 for s in range(1, n))).index(
        accept[s] if accept[s] is not None else -1) for s in range(1, n)]
    while True:
        sig = {}
        new = []
        for s in range(n):
            key = (block[s], tuple(block[t] for t in trans[s]))
            if s == 0:
                key = ('dead',)
            new.append(sig.setdefault(key, len(sig)))
        if len(set(new)) == len(set(block)):
            break
        block = new
    # Renumber: dead 0, start 1, the rest in order of first appearance.
    rename = {block[0]: 0, block[1]: 1}
    for s in range(n):
        if block[s] not in rename:
            rename[block[s]] = len(rename)
    m = len(rename)
    table = [[0] * len(alphabet) for _ in range(m)]
    acc = [0xFF] * m
    for s in range(n):
        r = rename[block[s]]
        table[r] = [rename[block[t]] for t in trans[s]]
        if accept[s] is not None:
            acc[r] = accept[s]
    cols = [other] * 64
    for v, c in column.items():
        cols[v] = c
    return table, acc, cols


def emit(name, machine):
    table, acc, cols = build(machine)
    assert len(table) < 256
    width = len(table[0])
    print('// %s: %d states, %d columns.' % (name, len(table), width))
    print('static const uint8_t k%sColumn[64] = {' % name)
    print('    ' + ', '.join(str(c) for c in cols) + '};')
    print('static const uint8_t k%sAccept[%d] = {' % (name, len(acc)))
    print('    ' + ', '.join('0x%02X' % a for a in acc) + '};')
    print('static const uint8_t k%sMoves[%d] = {' % (name, len(table) * width))
    for row in table:
        print('    ' + ', '.join(str(t) for t in row) + ',')
    print('};')


def main():
    print('// HarfBuzz\'s syllable grammars (MIT, see THIRD_PARTY_NOTICES) as minimal DFAs:')
    print('// a category\'s column, each state\'s syllable type (0xFF: not an end), and')
    print('// the moves, a row per state. State 0 is dead, state 1 the start.')
    emit('Indic', INDIC)
    emit('Khmer', KHMER)
    emit('Myanmar', MYANMAR)
    emit('Use', USE)


main()
