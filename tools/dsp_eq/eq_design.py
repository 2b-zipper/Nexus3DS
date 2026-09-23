"""3-band EQ coefficient design shared by the DSP patch tests and (later) the ARM side.
Bands: bass = first-order low shelf @200 Hz, mids = RBJ peaking @1 kHz (Q 0.7071), highs = first-order high shelf @4 kHz.
Each band is expressed as a biquad [b0, b1, b2, a1, a2] normalised (a0 = 1), quantised to Q12 for the DSP.
The DSP stores [b0, b1, b2, -a1, -a2] (so it can just sum the products)."""
import math

FS = 32728.0
BASS_HZ, MIDS_HZ, HIGH_HZ, MIDS_Q = 200.0, 1000.0, 4000.0, 0.7071
QBITS = 12

def _shelf(low, gain_db, fc):
    # fc is the shelf midpoint (where the gain is half of the total, in dB); the pole/zero pair is
    # placed symmetrically around it (warped frequency K = tan(pi f / fs))
    v0 = 10 ** (abs(gain_db) / 20.0)          # boost design, inverted for cuts
    k_mid = math.tan(math.pi * fc / FS)
    k = k_mid / math.sqrt(v0) if low else k_mid * math.sqrt(v0)
    cut = gain_db < 0
    a1 = (k - 1) / (k + 1)
    if low:
        b0, b1 = (1 + v0 * k) / (1 + k), (v0 * k - 1) / (1 + k)
    else:
        b0, b1 = (v0 + k) / (1 + k), (k - v0) / (1 + k)
    if cut:                                   # inverse filter: swap numerator/denominator
        b0, b1, a1 = 1 / b0, a1 / b0, b1 / b0
    return [b0, b1, 0.0, a1, 0.0]

def _peak(gain_db, fc, q):
    a = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * fc / FS
    al = math.sin(w0) / (2 * q)
    c = math.cos(w0)
    b = [1 + al * a, -2 * c, 1 - al * a]
    d = [1 + al / a, -2 * c, 1 - al / a]
    return [b[0] / d[0], b[1] / d[0], b[2] / d[0], d[1] / d[0], d[2] / d[0]]

def design(bass_db, mids_db, high_db):
    return [_shelf(True, bass_db, BASS_HZ), _peak(mids_db, MIDS_HZ, MIDS_Q), _shelf(False, high_db, HIGH_HZ)]

def q(v):
    r = int(round(v * (1 << QBITS)))
    assert -32768 <= r <= 32767, v
    return r & 0xFFFF

def dsp_words(bands):
    """per band [b0, b1, b2, -a1, -a2, -a1, -a2] as u16 words (Q12); the a terms appear twice (high/low half of y history)"""
    out = []
    for b0, b1, b2, a1, a2 in bands:
        out.append([q(b0), q(b1), q(b2), q(-a1), q(-a2), q(-a1), q(-a2)])
    return out

def response_db(bands, f, quantised=True):
    """magnitude response of the cascade at frequency f (Hz)"""
    w = 2 * math.pi * f / FS
    z1 = complex(math.cos(-w), math.sin(-w)); z2 = z1 * z1
    h = 1.0 + 0j
    for b0, b1, b2, a1, a2 in bands:
        if quantised:
            s = lambda v: (q(v) if q(v) < 32768 else q(v) - 65536) / float(1 << QBITS)
            b0, b1, b2, a1, a2 = s(b0), s(b1), s(b2), s(a1), s(a2)
        h *= (b0 + b1 * z1 + b2 * z2) / (1 + a1 * z1 + a2 * z2)
    return 20 * math.log10(abs(h))

if __name__ == '__main__':
    for gains in [(24, 0, 0), (-24, 0, 0), (0, 24, 0), (0, -24, 0), (0, 0, 24), (0, 0, -24), (12, -6, 18)]:
        bd = design(*gains)
        print(gains, [round(response_db(bd, f), 2) for f in (50, 100, 200, 500, 1000, 2000, 4000, 8000, 16000)])
