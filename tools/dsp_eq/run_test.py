import array, math, os, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__) or '.')
import eq_design as ed
S = os.environ.get('DSPEQ_WORK', '.')
H = os.environ.get('DSPEQ_HARNESS', './harness')
EQ = 0xD200

def poke_file(gains, path):
    words = ed.dsp_words(ed.design(*gains))
    with open(path, 'w') as f:
        for band in range(3):
            for i, w in enumerate(words[band]):
                f.write('%x %x\n' % (EQ + 0x10 + 8 * band + i, w))
        f.write('%x %x\n' % (EQ, 0xE0E1))

def run(fw, gains, k, amp=1000, frames=160, tag='t', chan=(1, 1)):
    env = dict(os.environ, EQ_AMP=str(amp), EQ_K=str(k), EQ_L=str(chan[0]), EQ_R=str(chan[1]))
    if gains is not None:
        poke_file(gains, os.path.join(S, 'poke_%s.txt' % tag))
        env['EQ_POKE'] = os.path.join(S, 'poke_%s.txt' % tag)
    out = os.path.join(S, 'out_' + tag)
    r = subprocess.run([H, fw, str(frames), out], env=env, capture_output=True, text=True, timeout=300)
    a = array.array('h'); a.frombytes(open(out + '_out.raw', 'rb').read())
    return a

def rms(a, ch, start=6000, n=8192):
    xs = [a[2 * i + ch] for i in range(start // 2, min(len(a) // 2, start // 2 + n))]
    return math.sqrt(sum(x * x for x in xs) / len(xs))

if __name__ == '__main__':
    fw = sys.argv[1]
    gains = tuple(float(x) for x in sys.argv[2].split(',')) if len(sys.argv) > 2 and sys.argv[2] != 'none' else None
    for k in (2, 8, 16, 32, 64, 128, 256, 512, 1024):
        f = k * ed.FS / 4096
        a = run(fw, gains, k)
        base = 1000 / math.sqrt(2)
        g = [20 * math.log10(max(rms(a, c), 1e-9) / base) for c in (0, 1)]
        exp = ed.response_db(ed.design(*gains), f) if gains else 0.0
        print('f=%7.1f Hz  measured L %+6.2f R %+6.2f dB  expected %+6.2f dB' % (f, g[0], g[1], exp))
