// Compiles the real Rosalina equalizer sources for the host and exposes what the emulator harness needs.
#include "../../../sysmodules/rosalina/source/equalizer.c"
#include "../../../sysmodules/rosalina/source/dsp_eq.c"

Result MyThread_Create(MyThread *t, void (*ep)(void), void *stack, u32 size, int prio, int aff) { (void)t; (void)ep; (void)stack; (void)size; (void)prio; (void)aff; return 0; }

u8 *g_dspRam;
u8 g_pdnDspCnt = 3;
bool preTerminationRequested;

bool g_hostHeadset;

void hosttest_set_profile_gains(int profile, int bass, int mids, int high)
{
    Equalizer_SetGain((EqProfile)profile, EQ_BAND_BASS, bass);
    Equalizer_SetGain((EqProfile)profile, EQ_BAND_MIDS, mids);
    Equalizer_SetGain((EqProfile)profile, EQ_BAND_HIGHS, high);
    DspEq_NotifyChanged();
}

// same gains for both outputs
void hosttest_set_gains(int bass, int mids, int high)
{
    hosttest_set_profile_gains(EQ_PROFILE_SPEAKERS, bass, mids, high);
    hosttest_set_profile_gains(EQ_PROFILE_HEADPHONES, bass, mids, high);
}

void hosttest_set_headset(int connected)
{
    g_hostHeadset = connected != 0;
}

void hosttest_tick(u8 *dspRam)
{
    g_dspRam = dspRam;
    DspEq_Tick();
}

void hosttest_status(u8 *dspRam, unsigned *hook, unsigned *magic)
{
    g_dspRam = dspRam;
    DspEqStatus st;
    DspEq_GetStatus(&st);
    *hook = st.hookA;
    *magic = st.magic;
}
