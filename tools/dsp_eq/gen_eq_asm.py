"""Generates the DSP-side 3-band EQ routine as makedsp1 assembly text.

Hook: the firmware writes the final mix (160 stereo s16 frames) into its BTDMP ring buffer either with `call 0x3432`
(plain copy, at 0x2FEB) or with `call 0x5DB4` (soft clipping, at 0x2FF1; the default clipping mode). The operand of each of those
two calls is redirected to ENTRY_A / ENTRY_B (a single 16-bit write each, so patching a running DSP is atomic). Both entries run
the original routine and then BODY, which - if the EQ block in DSP data memory holds the magic word - filters the freshly written
ring buffer (r4 = its start) in place: 3 cascaded biquad sections x 2 channels, direct form I, Q12 coefficients.
"""
import sys

CODE_BASE = 0x1000        # program memory address of the new segment (free area 0xF2E..0x28FF)
EQ = 0x8100               # data memory block (words): padding at the start of the DSP shared frame block
MAGIC = 0xE0E1
CF = lambda band: EQ + 0x10 + 8 * band
ST = lambda band, ch: EQ + 0x40 + 8 * (band * 2 + ch)
# diagnostics the routine keeps for Rosalina to display
DIAG_CALLS = EQ + 0x70   # number of times the routine ran
DIAG_R4 = EQ + 0x71      # buffer pointer (r4) it received the last time
DIAG_A = EQ + 0x72       # calls through the plain copy path
DIAG_B = EQ + 0x73       # calls through the soft clipping path
DIAG_IDX0 = EQ + 0x74    # firmware's output read index ([0x07ec], advances 16 words per 0.24 ms) when the routine started ...
DIAG_IDX1 = EQ + 0x75    # ... and when it finished (the difference is how long the equalizer took)
RING_IDX = 0x07EC
DIAG_CF = EQ + 0x80      # copy of the bass band's 7 coefficient words as the DSP read them ...
DIAG_ST = EQ + 0x88      # ... and of the bass band's left channel filter state after the last frame
DIAG_T = EQ + 0xB0       # self-test results (see below)
TEST_CF = EQ + 0xA0      # fixed self-test inputs
TEST_ST = EQ + 0xA8
DIAG_MOD = EQ + 0x90     # mod0..mod3, stt0..stt2 as received, then mod0 again after the product shift was cleared
HOOK_A = (0x2FEC, 0x3432)   # (program address of the call operand, original target): plain copy
HOOK_B = (0x2FF2, 0x5DB4)   # soft clipping

def mac_chain(a):
    """7 taps of  sum(c[i] * s[i])  with r0 -> coefficients, r4 -> state vector, a0 = signed taps, a1 = low-half taps.
    The real DSP cannot read two operands from the same memory bank in one instruction (it returned the same word for both),
    so every multiply reads memory once: the coefficient goes through y0 first."""
    a.ins('mov [r0++] y0'); a.ins('mpy y0 [r4++] a0')      # P = c0*s0
    for _ in range(4):
        a.ins('mov [r0++] y0'); a.ins('mac y0 [r4++] a0')  # a0 += P ; P = c_i*s_i  (i = 1..4)
    a.ins('mov [r0++] y0'); a.ins('macsu y0 [r4++] a0')    # a0 += P(4) ; P = c5*s5 (s5 = low half of y1: unsigned)
    a.ins('mov [r0++] y0'); a.ins('macsu y0 [r4++] a1')    # a1 += P(5) ; P = c6*s6
    a.ins('mac y0 [r4] a1')                                # a1 += P(6)

class Asm:
    def __init__(self, base):
        self.base = base
        self.items = []   # (kind, text, words)
        self.labels = {}
    def label(self, name):
        self.items.append(('label', name, 0))
    def ins(self, text, words=1):
        self.items.append(('ins', text, words))
    def resolve(self):
        pc = self.base
        for kind, text, words in self.items:
            if kind == 'label':
                self.labels[text] = pc
            else:
                pc += words
        self.end = pc
    def text(self):
        self.resolve()
        out = ['segment p %04x' % self.base]
        pc = self.base
        for kind, text, words in self.items:
            if kind == 'label':
                continue
            t = text
            for name, addr in self.labels.items():
                t = t.replace('@%s@' % name, '%04x' % addr)
                t = t.replace('@%s_last@' % name, '%04x' % (addr - 1))
            out.append(t)
            pc += words
        return '\n'.join(out) + '\n'

def gen():
    a = Asm(CODE_BASE)
    def bump(addr):                                # a0 is dead here (the callers reload it)
        a.ins('mov [0x$%04x] a0' % addr, 2)
        a.ins('add 0x$0001 a0', 2)
        a.ins('mov a0l [0x$%04x]' % addr, 2)
    a.label('ENTRY_A')
    a.ins('call 0x0000$%04x always' % HOOK_A[1], 2)
    bump(DIAG_A)
    a.ins('br 0x0000$@BODY@ always', 2)
    a.label('ENTRY_B')
    a.ins('call 0x0000$%04x always' % HOOK_B[1], 2)
    bump(DIAG_B)
    a.ins('br 0x0000$@BODY@ always', 2)
    a.label('BODY')                                # r4 = start of the 160-frame stereo buffer just written (both callees preserve it)
    a.ins('push r0'); a.ins('push r1'); a.ins('push r2'); a.ins('push r4'); a.ins('push y0')
    bump(DIAG_CALLS)
    for i, (enc, reg) in enumerate([('mod0', 'mod0'), ('mod1', 'mod1'), ('mod2', 'mod2'), ('mod3', 'mod3'), ('stt0', 'stt0'), ('stt1', 'stt1'), ('stt2', 'stt2')]):
        a.ins('mov %s a0l' % reg)
        a.ins('mov a0l [0x$%04x]' % (DIAG_MOD + i), 2)
    a.ins('push mod0')                              # the callee sets a product shift (load ps01) - do not depend on it
    a.ins('load 0x0000 ps01')                       # products unshifted, as the filter maths assumes
    a.ins('mov mod0 a0l')
    a.ins('mov a0l [0x$%04x]' % (DIAG_MOD + 7), 2)
    a.ins('mov r4 a0')
    a.ins('mov a0l [0x$%04x]' % DIAG_R4, 2)
    a.ins('mov [0x$%04x] a0' % RING_IDX, 2)
    a.ins('mov a0l [0x$%04x]' % DIAG_IDX0, 2)
    a.ins('mov [0x$%04x] a0' % EQ, 2)
    a.ins('cmpv 0x$%04x a0l' % MAGIC, 2)
    a.ins('br 0x0000$@DONE@ neq', 2)
    a.ins('mov r4 r2')
    # ---- self-test: the filter's multiply-accumulate chain on fixed inputs, intermediate results stored for Rosalina to show ----
    for i, w in enumerate([0x1012, 0xF09C, 0x0000, 0x0F76, 0x0000, 0x0F76, 0x0000]):
        a.ins('mov 0x$%04x a0' % w, 2)
        a.ins('mov a0l [0x$%04x]' % (TEST_CF + i), 2)
    for i, w in enumerate([0xFE44, 0xFE44, 0xFE58, 0x0000, 0x0000, 0x8000, 0x0000]):   # x0 x1 x2 y1h y2h y1l y2l
        a.ins('mov 0x$%04x a0' % w, 2)
        a.ins('mov a0l [0x$%04x]' % (TEST_ST + i), 2)
    a.ins('mov 0x$%04x r4' % TEST_ST, 2)
    a.ins('mov 0x$%04x r0' % TEST_CF, 2)
    a.ins('clr a0 always')
    a.ins('clr a1 always')
    mac_chain(a)
    a.ins('mov 0x$%04x r1' % DIAG_T, 2)
    a.ins('mov a0l [r1++]'); a.ins('mov a0h [r1++]')       # t0,t1: a0 after the 5 signed taps + macus
    a.ins('mov a1l [r1++]'); a.ins('mov a1h [r1++]')       # t2,t3: a1 (low taps)
    a.ins('shfi a1 a1 -0x0010')
    a.ins('mov a1l [r1++]'); a.ins('mov a1h [r1++]')       # t4,t5: a1 after >> 16
    a.ins('add a1 a0')
    a.ins('mov a0l [r1++]'); a.ins('mov a0h [r1++]')       # t6,t7: a0 after adding
    a.ins('shfi a0 a0 +0x0004')
    a.ins('lim a0 a0')
    a.ins('mov a0l [r1++]'); a.ins('mov a0h [r1++]')       # t8,t9: final
    for band in range(3):
        for ch in range(2):
            lbl = 'L_%d_%d' % (band, ch)
            a.ins('mov r2 r1')
            if ch:
                a.ins('addv 0x$0001 r1', 2)
            a.ins('bkrep 0x009fu8 0x0000$@%s_last@' % lbl, 2)
            a.ins('mov [r1] a1')                                   # x0
            a.ins('mov a1l [0x$%04x]' % ST(band, ch), 2)           # S[0] = x0
            a.ins('mov 0x$%04x r4' % ST(band, ch), 2)
            a.ins('mov 0x$%04x r0' % CF(band), 2)
            a.ins('clr a0 always')
            a.ins('clr a1 always')
            # S = [x0, x1, x2, y1h, y2h, y1l, y2l]   C = [b0, b1, b2, -a1, -a2, -a1, -a2]  (Q12)
            mac_chain(a)
            a.ins('shfi a1 a1 -0x0010')                            # low-part products are scaled by 2^16
            a.ins('add a1 a0')
            a.ins('shfi a0 a0 +0x0004')                            # Q12 -> Q16: sample in a0h, fraction in a0l
            a.ins('lim a0 a0')
            # state update: x2 = x1, x1 = x0, y2 = y1 (h and l), y1 = y (32 bit)
            for src, dst in ((3, 4), (5, 6), (1, 2), (0, 1)):
                a.ins('mov [0x$%04x] a1' % (ST(band, ch) + src), 2)
                a.ins('mov a1l [0x$%04x]' % (ST(band, ch) + dst), 2)
            a.ins('mov 0x$%04x r4' % (ST(band, ch) + 3), 2)
            a.ins('mov a0h [r4]')                                  # y1h
            a.ins('mov 0x$%04x r4' % (ST(band, ch) + 5), 2)
            a.ins('mov a0l [r4]')                                  # y1l
            a.ins('mov a0h [r1++]')                                # output sample
            a.label(lbl + '_pre')
            a.ins('modr [r1++]')                                   # skip the other channel
            a.label(lbl)                                           # label after last instruction
    for i in range(7):                             # diagnostics (only runs when the equalizer is active)
        a.ins('mov [0x$%04x] a0' % (CF(0) + i), 2)
        a.ins('mov a0l [0x$%04x]' % (DIAG_CF + i), 2)
        a.ins('mov [0x$%04x] a0' % (ST(0, 0) + i), 2)
        a.ins('mov a0l [0x$%04x]' % (DIAG_ST + i), 2)
    a.label('DONE')
    a.ins('mov [0x$%04x] a0' % RING_IDX, 2)
    a.ins('mov a0l [0x$%04x]' % DIAG_IDX1, 2)
    a.ins('pop mod0')
    a.ins('pop y0'); a.ins('pop r4'); a.ins('pop r2'); a.ins('pop r1'); a.ins('pop r0')
    a.ins('ret always')
    return a

if __name__ == '__main__':
    a = gen()
    txt = a.text()
    open(sys.argv[1] if len(sys.argv) > 1 else 'eq.asm', 'w').write(txt)
    print('words:', a.end - a.base, 'labels:', {k: hex(v) for k, v in a.labels.items() if not k.endswith('_pre')})
