// Minimal LLE test harness: boots a DSP1 firmware (dspfirm.cdc) in Teakra and plays a test tone through it, using the
// same pipe/shared-memory protocol as libctru's ndsp. Writes the DSP output to <prefix>_out.raw (stereo s16).
//
// usage: harness dspfirm.cdc <frames> [<prefix>]
// env:   EQ_AMP tone amplitude, EQ_K tone cycles per 4096 samples, EQ_L / EQ_R voice L/R gain,
//        EQ_POKE file with "addr(hex) value(hex)" lines written to DSP data memory (word addresses) after boot,
//        EQ_VOICES number of voices, EQ_FX enable aux/effects, EQ_BQ enable a voice biquad filter
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include <teakra/teakra.h>

#ifdef DSPEQ_HOST_GLUE   // link host_test/host_glue.c: runs the real Rosalina equalizer code against the emulated DSP RAM
extern "C" { void hosttest_set_gains(int, int, int); void hosttest_tick(unsigned char*); void hosttest_status(unsigned char*, unsigned*, unsigned*); }
#endif
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t;

static constexpr u32 DataOff = 0x40000;
static constexpr u32 Slice = 16384;
static constexpr u32 FcramBase = 0x20000000;
static std::vector<u8> fcram(16 * 1024 * 1024);

static Teakra::Teakra* T;
static u16 pipe_base = 0;
static bool loaded = false;
static bool sem_sig = false, data_sig = false;
static int irq_pipe = -1;          // pending DSP->CPU pipe event
static std::vector<std::array<int16_t,2>> samples;

struct PipeStatus { u16 waddress, bsize, read_bptr, write_bptr; u8 slot, flags; };

static u8* D(u32 baddr) { return T->GetDspMemory() + DataOff + baddr; }
static PipeStatus GetPS(int pipe, int dir) {
    int slot = pipe * 2 + dir; PipeStatus ps; memcpy(&ps, D(pipe_base * 2 + slot * 10), 10); return ps; }
static void UpdPS(const PipeStatus& ps) {
    u8* a = D(pipe_base * 2 + ps.slot * 10);
    if (ps.slot % 2 == 0) memcpy(a + 4, &ps.read_bptr, 2); else memcpy(a + 6, &ps.write_bptr, 2); }

static void RunSlice() { T->Run(Slice); }

static void WritePipe(int pipe, const std::vector<u8>& data) {
    PipeStatus ps = GetPS(pipe, 1);
    const u8* p = data.data(); u16 n = data.size();
    while (n) {
        bool wrapped = ((ps.read_bptr ^ ps.write_bptr) >= 0x8000);
        u16 end = wrapped ? (ps.read_bptr & 0x7FFF) : ps.bsize;
        u16 begin = ps.write_bptr & 0x7FFF;
        u16 sz = std::min<u16>(n, end - begin);
        memcpy(D(ps.waddress * 2 + begin), p, sz);
        p += sz; ps.write_bptr += sz; n -= sz;
        if ((ps.write_bptr & 0x7FFF) == ps.bsize) { ps.write_bptr &= 0x8000; ps.write_bptr ^= 0x8000; }
    }
    UpdPS(ps);
    while (!T->SendDataIsEmpty(2)) RunSlice();
    T->SendData(2, ps.slot);
}
static std::vector<u8> ReadPipe(int pipe, u16 n) {
    PipeStatus ps = GetPS(pipe, 0); std::vector<u8> out(n); u8* p = out.data();
    while (n) {
        bool wrapped = ((ps.read_bptr ^ ps.write_bptr) >= 0x8000);
        u16 end = wrapped ? ps.bsize : (ps.write_bptr & 0x7FFF);
        u16 begin = ps.read_bptr & 0x7FFF;
        u16 sz = std::min<u16>(n, end - begin);
        memcpy(p, D(ps.waddress * 2 + begin), sz);
        p += sz; ps.read_bptr += sz; n -= sz;
        if ((ps.read_bptr & 0x7FFF) == ps.bsize) { ps.read_bptr &= 0x8000; ps.read_bptr ^= 0x8000; }
    }
    UpdPS(ps);
    while (!T->SendDataIsEmpty(2)) RunSlice();
    T->SendData(2, ps.slot);
    return out;
}
static u16 Readable(int pipe) {
    PipeStatus ps = GetPS(pipe, 0);
    u16 size = ps.write_bptr - ps.read_bptr;
    if ((ps.read_bptr ^ ps.write_bptr) >= 0x8000) size += ps.bsize;
    return size & 0x7FFF;
}

static void ProcessPipeEvent(bool from_data) {
    if (!loaded) return;
    if (from_data) data_sig = true;
    else { if ((T->GetSemaphore() & 0x8000) == 0) return; sem_sig = true; }
    if (sem_sig && data_sig) {
        sem_sig = data_sig = false;
        u16 slot = T->RecvData(2);
        int side = slot % 2, pipe = slot / 2;
        if (side != 0) return;
        if (pipe == 0) { ReadPipe(0, Readable(0)); return; }
        irq_pipe = pipe;
    }
}

static bool WaitIrq(int maxSlices = 4000) {
    irq_pipe = -1;
    for (int i = 0; i < maxSlices && irq_pipe < 0; ++i) RunSlice();
    return irq_pipe >= 0;
}

// ---- ndsp-style state
static void* vars[16][2];
static u16 frameId, bufCurId, bufId;
static void set16(u8* p, int o, u16 v) { memcpy(p + o, &v, 2); }
static void set32(u8* p, int o, u32 v) { memcpy(p + o, &v, 4); }
static void setf(u8* p, int o, float v) { memcpy(p + o, &v, 4); }
static u32 rot(u32 x) { return (x << 16) | (x >> 16); }
static u16 get16(u8* p, int o) { u16 v; memcpy(&v, p + o, 2); return v; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: harness dspfirm.cdc frames [dumpprefix]\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<u8> raw((std::istreambuf_iterator<char>(f)), {});
    int frames = atoi(argv[2]);

    Teakra::Teakra t(Teakra::UserConfig{}); T = &t;
    t.Reset();
    Teakra::AHBMCallback a;
    auto P = [](u32 addr) -> u8* { return &fcram[addr - FcramBase]; };
    a.read8 = [P](u32 x) { return *P(x); }; a.write8 = [P](u32 x, u8 v) { *P(x) = v; };
    a.read16 = [P](u32 x) { u16 v; memcpy(&v, P(x), 2); return v; };
    a.write16 = [P](u32 x, u16 v) { memcpy(P(x), &v, 2); };
    a.read32 = [P](u32 x) { u32 v; memcpy(&v, P(x), 4); return v; };
    a.write32 = [P](u32 x, u32 v) { memcpy(P(x), &v, 4); };
    t.SetAHBMCallback(a);
    t.SetAudioCallback([](std::array<int16_t,2> s) { samples.push_back(s); });

    // header
    u8 nseg = raw[0x10E]; u8 flags = raw[0x10F];
    bool recv_on_start = flags & 1;
    u8* mem = t.GetDspMemory();
    for (int i = 0; i < nseg; ++i) {
        u32 o = 0x120 + i * 0x30;
        u32 off, addr, size; memcpy(&off, &raw[o], 4); memcpy(&addr, &raw[o+4], 4); memcpy(&size, &raw[o+8], 4);
        u8 type = raw[o + 0xF];
        if (type <= 1) memcpy(mem + addr * 2, &raw[off], size);
        else memcpy(mem + DataOff + addr * 2, &raw[off], size);
        printf("seg %d type %d addr %05x size %05x\n", i, type, addr, size);
    }
    if (recv_on_start)
        for (u8 i = 0; i < 3; ++i) { do { while (!t.RecvDataIsReady(i)) RunSlice(); } while (t.RecvData(i) != 1); }
    while (!t.RecvDataIsReady(2)) RunSlice();
    pipe_base = t.RecvData(2);
    loaded = true;
    printf("pipe base waddr %04x\n", pipe_base);
    t.SetRecvDataHandler(2, []() { ProcessPipeEvent(true); });
    t.SetSemaphoreHandler([]() { ProcessPipeEvent(false); });

    // ndspInitialize
    WritePipe(2, {0, 0, 0, 0});
    t.SetSemaphore(0x4000);
    if (!WaitIrq()) { printf("no init irq\n"); return 2; }
    auto v = ReadPipe(2, 2); u16 cnt; memcpy(&cnt, v.data(), 2);
    auto vv = ReadPipe(2, cnt * 2);
    u16 vw[16]; memcpy(vw, vv.data(), cnt * 2);
    printf("pipe2 vars (%d):", cnt);
    for (int i = 0; i < cnt; ++i) { printf(" %04x", vw[i]); vars[i][0] = D(vw[i] * 2); vars[i][1] = D((vw[i] | 0x10000) * 2); }
    printf("\n");
    t.SetSemaphore(0x4000);
    frameId = 4; set16((u8*)vars[0][0], 0, 4); frameId++;
    t.SetSemaphore(0x2000);
    bufCurId = frameId & 1; bufId = frameId & 1;

    // test tone: 1 kHz-ish square/sine at 32728 Hz, mono PCM16, looping, in FCRAM at phys 0x20100000
    const int N = 4096; std::vector<int16_t> tone(N);
    double amp = getenv("EQ_AMP") ? atof(getenv("EQ_AMP")) : 12000.0; double kcyc = getenv("EQ_K") ? atof(getenv("EQ_K")) : 64.0;
    for (int i = 0; i < N; ++i) tone[i] = (int16_t)(amp * sin(2 * M_PI * kcyc * i / N));
    memcpy(&fcram[0x100000], tone.data(), N * 2);

    bool bq = getenv("EQ_BQ") != nullptr;
    if (getenv("EQ_POKE")) {
        FILE* pf = fopen(getenv("EQ_POKE"), "r"); unsigned pa, pv; int cntp = 0;
        while (pf && fscanf(pf, "%x %x", &pa, &pv) == 2) { u16 v16 = pv; memcpy(D(pa * 2), &v16, 2); ++cntp; }
        if (pf) fclose(pf);
        printf("poked %d words\n", cntp);
    }
    bool voiceSet = false;
    int lastPrintSamples = 0;
#ifdef DSPEQ_HOST_GLUE
    // EQ_LIVE="b,m,h@frame[;b,m,h@frame...]": set gains at that frame; hosttest_tick() runs every 20 frames (~100 ms)
    int liveB[8], liveM[8], liveH[8], liveF[8], nLive = 0;
    if (getenv("EQ_LIVE")) {
        char* e = strdup(getenv("EQ_LIVE"));
        for (char* tok = strtok(e, ";"); tok && nLive < 8; tok = strtok(nullptr, ";"), ++nLive)
            sscanf(tok, "%d,%d,%d@%d", &liveB[nLive], &liveM[nLive], &liveH[nLive], &liveF[nLive]);
    }
    bool liveTicking = false;
#endif
    for (int fr = 0; fr < frames; ++fr) {
#ifdef DSPEQ_HOST_GLUE
        for (int q = 0; q < nLive; ++q)
            if (fr == liveF[q]) { hosttest_set_gains(liveB[q], liveM[q], liveH[q]); liveTicking = true; }
        if (liveTicking && fr % 20 == 0) {
            hosttest_tick(T->GetDspMemory());
            unsigned hook, magic; hosttest_status(T->GetDspMemory(), &hook, &magic);
            if (fr % 100 == 0) printf("live frame %d: hook %04x magic %04x\n", fr, hook, magic);
        }
#endif
        if (!WaitIrq()) { printf("frame %d: no irq\n", fr); break; }
        u16 counter = get16((u8*)vars[0][(~frameId) & 1], 0);
        if (counter) {
            u16 next = counter + 1; frameId = next ? next : 2; bufId = frameId & 1;
        }
        // master status
        u8* m = (u8*)vars[4][bufCurId];
        u32 mf; memcpy(&mf, m, 4);
        mf |= 0x10000000 | 0x00010000 | 0x04000000 | 0x08000000 | 0x00008000;
        setf(m, 4, 1.0f); set16(m, 16, 2); set16(m, 22, getenv("EQ_MODE") ? atoi(getenv("EQ_MODE")) : 1); set16(m, 24, getenv("EQ_CLIP") ? atoi(getenv("EQ_CLIP")) : 0); set16(m, 26, 2);
        // header: headsetConnected at offset 30? keep 0
        if (getenv("EQ_FX") && fr >= 1) {   // aux buses + delay + reverb, aux return volumes
            mf |= 0x100 | 0x200 | 0x40 | 0x80 | 0x1000000 | 0x2000000 | 0x400 | 0x800 | 0x1000 | 0x2000;
            set16(m, 40, 1); set16(m, 42, 1); set16(m, 36, 0); set16(m, 38, 0);
            setf(m, 8, 1.0f); setf(m, 12, 1.0f);
            for (int i = 0; i < 2; ++i) { set16(m, 44 + i * 20, 1); set16(m, 84 + i * 52, 1); }
        }
        set32(m, 0, mf);
        if (!voiceSet && fr >= 2) {
            u8* c = (u8*)vars[1][frameId & 1];
            set32(c, 0, 0x20000000 | 0xE000000 | 0x40000 | 0x20000 | 0x10 | 0x40200000 | 0x10000);
            if (getenv("EQ_FX")) { setf(c, 20, 0.7f); setf(c, 24, 0.7f); setf(c, 36, 0.5f); setf(c, 40, 0.5f); }
            setf(c, 4, getenv("EQ_L") ? atof(getenv("EQ_L")) : 1.0f); setf(c, 8, getenv("EQ_R") ? atof(getenv("EQ_R")) : 1.0f);
            setf(c, 52, 1.0f);                            // rate
            c[56] = 1; c[57] = 1;                         // interp linear
            set32(c, 172, rot(FcramBase + 0x100000));
            set32(c, 176, rot(N));
            set16(c, 180, 5);                             // mono PCM16
            set16(c, 188, 2);                             // looping
            set16(c, 190, 1);                             // seq id
            set16(c, 160, 1);                             // play
            {   // extra voices (stress the per-voice state area)
                int nv = getenv("EQ_VOICES") ? atoi(getenv("EQ_VOICES")) : 1;
                for (int v = 1; v < nv && v < 24; ++v) {
                    u8* cv = (u8*)vars[1][frameId & 1] + v * 192;
                    memcpy(cv, c, 192);
                    setf(cv, 4, 0.3f + 0.02f * v); setf(cv, 8, 0.6f - 0.02f * v);
                    setf(cv, 52, 0.8f + 0.05f * v);
                    set16(cv, 190, 1 + v);
                }
            }
            if (bq) { set32(c, 0, 0x20000000 | 0xE000000 | 0x40000 | 0x20000 | 0x10 | 0x40200000 | 0x10000 | 0x400000 | 0x1000000);
                      set16(c, 58, 2); u16 co[5] = {0, 0, 0x1000, 0x2000, 0x1000}; memcpy(c + 64, co, 10); }
            voiceSet = true;
            printf("voice set at frame %d\n", fr);
        }
        set16((u8*)vars[0][bufCurId], 0, frameId); frameId++;
        t.SetSemaphore(0x2000);
        bufCurId = frameId & 1;
        if (fr % 50 == 49 || fr == frames - 1) {
            int maxAbs = 0; for (size_t i = lastPrintSamples; i < samples.size(); ++i) maxAbs = std::max(maxAbs, std::abs((int)samples[i][0]));
            printf("frame %d: samples so far %zu, peak L since last %d\n", fr, samples.size(), maxAbs);
            lastPrintSamples = samples.size();
        }
    }
    if (argc > 3) {
        std::string p = argv[3];
        std::ofstream o(p + "_dspram.bin", std::ios::binary); o.write((char*)t.GetDspMemory(), 0x80000);
        std::ofstream s(p + "_out.raw", std::ios::binary); s.write((char*)samples.data(), samples.size() * 4);
    }
    return 0;
}
