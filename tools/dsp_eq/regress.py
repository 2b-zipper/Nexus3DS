import run_test as r, eq_design as ed, math, sys, random
fw = sys.argv[1] if len(sys.argv) > 1 else 'dspfirm_eq.cdc'
random.seed(1)
cases = [(24,0,0),(-24,0,0),(0,24,0),(0,-24,0),(0,0,24),(0,0,-24),(24,24,24),(-24,-24,-24),(24,-24,24),(-24,24,-24),(12,-6,18),(-10,15,-3)]
worst = 0; n = 0
for gains in cases:
    row = []
    for k in (4, 16, 48, 96, 192, 384, 768, 1536):
        f = k * ed.FS / 4096
        exp = ed.response_db(ed.design(*gains), f)
        amp = min(max(4000 / 10 ** (exp / 20), 30), 30000)
        if amp * 10 ** (exp / 20) > 30000: continue
        a = r.run(fw, gains, k, amp=amp, frames=180, tag='r')
        m = 20 * math.log10(max(r.rms(a, 0, start=12000), 1e-9) / (amp / math.sqrt(2)))
        mr = 20 * math.log10(max(r.rms(a, 1, start=12000), 1e-9) / (amp / math.sqrt(2)))
        err = max(abs(m - exp), abs(mr - exp)); worst = max(worst, err); n += 1
        row.append('%.0fHz:%+.1f(%+.2f)' % (f, m, m - exp))
    print(gains, ' '.join(row))
print('points', n, 'worst |error| dB', round(worst, 2))
