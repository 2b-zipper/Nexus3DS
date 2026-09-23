"""Builds a patched DSP firmware: appends the EQ code segment and redirects the firmware's final-mix call to it."""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_eq_asm

def patch(src, eq_dsp1, dst):
    a = gen_eq_asm.gen(); a.resolve()
    hooks = [(gen_eq_asm.HOOK_A, a.labels['ENTRY_A']), (gen_eq_asm.HOOK_B, a.labels['ENTRY_B'])]
    fw = bytearray(open(src, 'rb').read())
    eq = open(eq_dsp1, 'rb').read()
    # new segment payload from the makedsp1 output
    e_off, e_addr, e_size = struct.unpack_from('<3I', eq, 0x120)
    payload = eq[e_off:e_off + e_size]

    nseg = fw[0x10E]
    segs = []
    for i in range(nseg):
        o = 0x120 + i * 0x30
        off, addr, size = struct.unpack_from('<3I', fw, o)
        segs.append((i, off, addr, size, fw[o + 0xF]))

    # 1. redirect the operands of the hooked calls, inside the program segment that covers them
    for (hook_word_addr, orig_target), new_target in hooks:
        for i, off, addr, size, mtype in segs:
            if mtype <= 1 and addr <= hook_word_addr < addr + size // 2:
                p = off + (hook_word_addr - addr) * 2
                assert struct.unpack_from('<H', fw, p)[0] == orig_target, 'unexpected hook word'
                struct.pack_into('<H', fw, p, new_target)
                sha = hashlib.sha256(bytes(fw[off:off + size])).digest()   # refresh this segment's sha256
                fw[0x120 + i * 0x30 + 0x10:0x120 + i * 0x30 + 0x30] = sha
                break
        else:
            raise SystemExit('hook address not found in any program segment')

    # 2. append the EQ segment
    assert nseg < 10
    total = struct.unpack_from('<I', fw, 0x104)[0]
    assert total == len(fw), (total, len(fw))
    o = 0x120 + nseg * 0x30
    struct.pack_into('<3I', fw, o, len(fw), e_addr, e_size)
    fw[o + 0xC:o + 0xF] = b'\0\0\0'
    fw[o + 0xF] = 0                                   # program memory
    fw[o + 0x10:o + 0x30] = hashlib.sha256(payload).digest()
    fw += payload
    fw[0x10E] = nseg + 1
    struct.pack_into('<I', fw, 0x104, len(fw))
    open(dst, 'wb').write(fw)
    return len(fw)

if __name__ == '__main__':
    print(patch(sys.argv[1], sys.argv[2], sys.argv[3]))
