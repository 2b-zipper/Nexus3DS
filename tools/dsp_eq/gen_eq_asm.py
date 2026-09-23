"""Generates the DSP-side 3-band EQ routine as makedsp1 assembly text.

Hook: the firmware's `call 0x3432` (final mix -> BTDMP ring buffer, 160 stereo s16 frames) at 0x2FEB is redirected to EQ_ENTRY.
EQ_ENTRY runs the original routine, then (if the EQ block in DSP data memory holds the magic word) filters
the freshly written ring buffer in place: 3 cascaded biquad sections x 2 channels, direct form I, Q12 coefficients.
"""
import sys

CODE_BASE = 0x1000        # program memory address of the new segment (free area 0xF2E..0x28FF)
EQ = 0xD200               # data memory block (words): verified free on a real console dump + emulator runs
MAGIC = 0xE0E1
CF = lambda band: EQ + 0x10 + 8 * band
ST = lambda band, ch: EQ + 0x40 + 8 * (band * 2 + ch)
ORIG_CALL_TARGET = 0x3432
RETURN_TO = 0x2FED

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
    a.label('ENTRY')
    a.ins('push r2')                               # r2 is live in the caller
    a.ins('push b0l')                              # destination pointer of the original call (b0 = r4)
    a.ins('call 0x0000$%04x always' % ORIG_CALL_TARGET, 2)
    a.ins('pop r2')                                # r2 = start of the freshly written 160-frame stereo buffer
    a.ins('push r0'); a.ins('push r1'); a.ins('push r4'); a.ins('push y0')
    a.ins('mov [0x$%04x] a0' % EQ, 2)
    a.ins('cmpv 0x$%04x a0l' % MAGIC, 2)
    a.ins('br 0x0000$@DONE@ neq', 2)
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
            a.ins('mpy [r4++] [r0++] a0')                          # P = b0*x0
            a.ins('mac [r4++] [r0++] a0')                          # a0 += P ; P = b1*x1
            a.ins('mac [r4++] [r0++] a0')                          # P = b2*x2
            a.ins('mac [r4++] [r0++] a0')                          # P = -a1*y1h
            a.ins('mac [r4++] [r0++] a0')                          # P = -a2*y2h
            a.ins('macus [r4++] [r0++] a0')                        # a0 += P ; P = -a1*y1l (y1l unsigned)
            a.ins('macus [r4++] [r0++] a1')                        # a1 += P(-a1*y1l) ; P = -a2*y2l
            a.ins('mac [r4++] [r0++] a1')                          # a1 += P(-a2*y2l)
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
    a.label('DONE')
    a.ins('pop y0'); a.ins('pop r4'); a.ins('pop r1'); a.ins('pop r0')
    a.ins('pop r2')
    a.ins('ret always')
    return a

if __name__ == '__main__':
    a = gen()
    txt = a.text()
    open(sys.argv[1] if len(sys.argv) > 1 else 'eq.asm', 'w').write(txt)
    print('words:', a.end - a.base, 'labels:', {k: hex(v) for k, v in a.labels.items() if not k.endswith('_pre')})
