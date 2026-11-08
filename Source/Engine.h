#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace wings
{
constexpr float kPi = 3.14159265358979f;

struct SVF
{
    float ic1 = 0, ic2 = 0, g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
    void set (float fc, float Q, float sr)
    {
        fc = juce::jlimit (10.0f, sr * 0.45f, fc);
        g = std::tan (kPi * fc / sr);
        k = 1.0f / juce::jmax (0.1f, Q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // lp, bp (unity peak), hp
    inline void process (float v0, float& lp, float& bp, float& hp)
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        lp = v2;
        bp = k * v1;
        hp = v0 - k * v1 - v2;
    }
    void reset() { ic1 = ic2 = 0; }
};

struct Rng
{
    uint32_t s = 22222u;
    inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float uni() { return (float) (next() & 0xFFFFFF) / 16777216.0f; }       // 0..1
    inline float bi() { return uni() * 2.0f - 1.0f; }                                // -1..1
};

// enveloppe exponentielle par segments (cible, durée en secondes)
struct Env
{
    float v = 0.0001f, mul = 1.0f;
    int seg = 0, left = 0, nseg = 0;
    float tgt[4] {}, dur[4] {};
    float sr = 48000.0f;
    bool linearEnd = false;
    void clear() { nseg = 0; seg = 0; left = 0; v = 0.0001f; mul = 1; linearEnd = false; }
    void add (float target, float seconds) { tgt[nseg] = juce::jmax (0.0001f, target); dur[nseg] = seconds; ++nseg; }
    void begin()
    {
        seg = 0; v = 0.0001f; startSeg();
    }
    void startSeg()
    {
        if (seg >= nseg) { left = 0; mul = 1; return; }
        left = juce::jmax (1, (int) (dur[seg] * sr));
        mul = std::pow (tgt[seg] / juce::jmax (0.00001f, v), 1.0f / (float) left);
    }
    inline float next()
    {
        if (seg >= nseg) return 0.0f;
        v *= mul;
        if (--left <= 0) { v = tgt[seg]; ++seg; startSeg(); }
        return v;
    }
    bool done() const { return seg >= nseg; }
};

struct Params
{
    std::atomic<float> vol { 0.f }, rev { 55.f }, fb { 30.f }, fbT { 50.f }, pitch { 0.f }, olap { 40.f }, curve { 60.f }, slw { 20.f },
        pulse { 25.f }, pulseW { 50.f }, res { 35.f };
    std::array<std::atomic<float>, 5> n {};
};

struct Event
{
    int type = 0; // 1 note, 2 glitch, 3 riser start, 4 riser stop, 5 drop
    float a = 0, b = 0, c = 0, d = 0, e = 0;
    int mask = 0;
};

class Engine
{
public:
    Params params;

    void prepare (double sampleRate)
    {
        sr = (float) sampleRate;
        for (auto& v : notes) v.active = false;
        for (auto& v : sfx) v.active = false;
        const int dmax = (int) (sr * 1.0f) + 8;
        for (int c = 0; c < 2; ++c) { fbBuf[c].assign ((size_t) dmax, 0.0f); loopBuf[c].assign ((size_t) (sr * 0.5f) + 8, 0.0f); }
        fbW = 0; loopW = 0;
        reverb.setSampleRate (sampleRate);
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.93f; rp.damping = 0.45f; rp.wetLevel = 1.0f; rp.dryLevel = 0.0f; rp.width = 1.0f; rp.freezeMode = 0.0f;
        reverb.setParameters (rp);
        hpL.reset(); hpR.reset(); hpL.set (160.0f, 0.5f, sr); hpR.set (160.0f, 0.5f, sr);
        fbLpL.reset(); fbLpR.reset(); bpFx.reset();
        compEnv = 0; ringPh = 0; held = 0;
    }

    void push (const Event& e)
    {
        const juce::SpinLock::ScopedLockType l (lock);
        if (queue.size() < 4096) queue.push_back (e);
    }

    void noteOnMidi (int midi, float vel)
    {
        Event e; e.type = 1; e.a = (float) midi; e.b = vel; e.c = 0.0f; e.d = 0.4f; e.e = 0.0f; push (e);
    }

    void render (float* L, float* R, int n)
    {
        {
            const juce::SpinLock::ScopedTryLockType l (lock);
            if (l.isLocked() && ! queue.empty()) { local.swap (queue); queue.clear(); }
        }
        for (auto& e : local) handle (e);
        local.clear();

        const float vol = params.vol.load();
        const float masterG = 0.5f * std::pow (10.0f, vol / 20.0f);
        const float revSend = params.rev.load() / 100.0f;
        const float fbAmt = params.fb.load() / 100.0f, fbT = params.fbT.load() / 100.0f;
        const float fbGain = fbAmt * 0.82f, fbOutG = fbAmt * 100.0f > 1.0f ? 0.6f : 0.0f;
        const int dlen = juce::jlimit (16, (int) fbBuf[0].size() - 2, (int) ((0.06f + fbT * 0.34f) * sr));
        const float fbLpF = 800.0f + (1.0f - fbT) * 7000.0f;
        fbLpL.set (fbLpF, 0.7071f, sr); fbLpR.set (fbLpF, 0.7071f, sr);
        const float glitchW = 1.0f;
        (void) glitchW;

        for (int i = 0; i < n; ++i)
        {
            float dryL = 0, dryR = 0;
            for (auto& v : notes) if (v.active) v.tick (*this, dryL, dryR);
            for (auto& v : sfx) if (v.active) v.tick (*this, dryL, dryR);

            // écho à feedback
            const int rp = (fbW - dlen + (int) fbBuf[0].size()) % (int) fbBuf[0].size();
            float dl = fbBuf[0][(size_t) rp], dr = fbBuf[1][(size_t) rp], l1, b1, h1, l2, b2, h2;
            fbLpL.process (dl, l1, b1, h1); fbLpR.process (dr, l2, b2, h2);
            fbBuf[0][(size_t) fbW] = dryL + l1 * fbGain;
            fbBuf[1][(size_t) fbW] = dryR + l2 * fbGain;
            fbW = (fbW + 1) % (int) fbBuf[0].size();
            const float fol = l1 * fbOutG, forr = l2 * fbOutG;

            const float toRevL = (dryL + fol) * revSend, toRevR = (dryR + forr) * revSend;
            float rl = toRevL, rr = toRevR;
            reverb.processStereo (&rl, &rr, 1);
            float mL = (dryL + fol + rl * 0.9f) * masterG, mR = (dryR + forr + rr * 0.9f) * masterG;

            // glitch bus
            tickGlitch (mL, mR);

            // highpass 160 Hz
            float lp, bp, hp;
            hpL.process (mL, lp, bp, hp); mL = hp;
            hpR.process (mR, lp, bp, hp); mR = hp;

            // compresseur
            const float lvl = juce::jmax (std::abs (mL), std::abs (mR));
            const float coef = lvl > compEnv ? 0.0f + std::exp (-1.0f / (0.003f * sr)) : std::exp (-1.0f / (0.25f * sr));
            compEnv = lvl + coef * (compEnv - lvl);
            const float db = 20.0f * std::log10 (juce::jmax (1.0e-6f, compEnv));
            const float thr = -14.0f, knee = 12.0f, ratio = 4.0f;
            float over = db - thr, red = 0.0f;
            if (2 * over < -knee) red = 0.0f;
            else if (2 * std::abs (over) <= knee) red = (1.0f - 1.0f / ratio) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            else red = (1.0f - 1.0f / ratio) * over;
            const float cg = std::pow (10.0f, -red / 20.0f);
            L[i] += std::tanh (mL * cg * 1.0f);
            R[i] += std::tanh (mR * cg * 1.0f);
        }
    }

private:
    // ---------------- voix de notes ----------------
    struct NoteVoice
    {
        bool active = false;
        int delay = 0;
        float f = 440, fcur = 440, fmul = 1, fsweepLeft = 0, f2 = 0, ph = 0, ph2 = 0, ph3 = 0, ph4 = 0;
        float vel = 0.5f, mix = 0, pw = 0.25f, panL = 0.7f, panR = 0.7f, click = 0, accG = 0;
        float dec = 0.2f, clickAmp = 0;
        int age = 0, total = 0, clickLeft = 0;
        Env envS, envH, envA, envP, envP2;
        SVF bp, lp, hpClick;
        bool hasPulse = false, acc = false;

        static float blep (float t, float dt)
        {
            if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
            if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
            return 0.0f;
        }
        void tick (Engine& E, float& oL, float& oR)
        {
            if (delay > 0) { --delay; return; }
            const float sr = E.sr;
            if (fsweepLeft > 0) { fcur *= fmul; fsweepLeft -= 1; if (fsweepLeft <= 0) fcur = f; }
            const float dp = fcur / sr;
            ph += dp; if (ph >= 1) ph -= 1;
            ph2 += dp * 2.76f / 1.0f; ph2 -= std::floor (ph2);
            float s = std::sin (2 * kPi * ph) * envS.next() ;
            s += std::sin (2 * kPi * ph2) * envH.next();
            if (acc) { ph3 += dp * 2.0f; ph3 -= std::floor (ph3); s += std::sin (2 * kPi * ph3) * envA.next(); }
            if (hasPulse)
            {
                ph4 += dp; ph4 -= std::floor (ph4);
                float p = (ph4 < pw ? 1.0f : -1.0f) + blep (ph4, dp) - blep (std::fmod (ph4 - pw + 1.0f, 1.0f), dp) - (2 * pw - 1.0f);
                float l1, b1, h1, l2, b2, h2;
                const float e1 = envP.next(), e2 = envP2.next();
                bp.process (p * e1, l1, b1, h1);
                lp.process (b1 + p * e2, l2, b2, h2);
                s += l2;
            }
            if (clickLeft > 0)
            {
                const float nz = E.rng.bi() * ((float) clickLeft / (float) (0.01f * sr));
                float l1, b1, h1; hpClick.process (nz, l1, b1, h1); s += h1 * clickAmp; --clickLeft;
            }
            oL += s * panL; oR += s * panR;
            if (envS.done() && envH.done() && (! hasPulse || envP.done())) active = false;
        }
    };

    // ---------------- voix sfx génériques ----------------
    struct SfxVoice
    {
        bool active = false;
        int delay = 0;
        int osc = 0;          // 0 sine 1 saw 2 square 3 noise
        float f = 440, fmul = 1, fleft = 0, ph = 0;
        int ftype = 0;        // 0 aucun 1 lp 2 bp 3 hp
        float c = 1000, cmul = 1, cleft = 0, Q = 1;
        SVF filt;
        Env env; float peak = 0.1f; int emode = 0; // 0 decay, 1 rise, 2 held (riser) ; released = true -> release
        float rise = 0, relCoef = 0; bool released = false, holding = false;
        float panL = 0.7f, panR = 0.7f, revSend = 0.0f, dryAmt = 1.0f;
        float level = 0.0001f; float up = 1.0f;
        float riseMul = 1.0f; int riseLeft = 0; int total = 0; int age = 0; int mode = 0;
        float gain = 0.0001f, gainMul = 1.0f; int seg = 0; int segLeft = 0; float decMul = 1.0f; float attMul = 1.0f; int attLeft = 0;
        float linStep = 0, dur = 0.2f;

        void tick (Engine& E, float& oL, float& oR)
        {
            if (delay > 0) { --delay; return; }
            if (fleft > 0) { f *= fmul; fleft -= 1; }
            if (cleft > 0) { c *= cmul; cleft -= 1; filt.set (c, Q, E.sr); }
            float x = 0;
            if (osc == 3) x = E.rng.bi();
            else
            {
                ph += f / E.sr; ph -= std::floor (ph);
                if (osc == 0) x = std::sin (2 * kPi * ph);
                else if (osc == 1) x = 2 * ph - 1;
                else x = ph < 0.5f ? 1.0f : -1.0f;
            }
            if (ftype != 0)
            {
                float l1, b1, h1; filt.process (x, l1, b1, h1);
                x = ftype == 1 ? l1 : (ftype == 2 ? b1 : h1);
            }
            // gain
            float g = 0;
            if (mode == 0) // attaque 2 ms puis décroissance exponentielle
            {
                if (attLeft > 0) { gain *= attMul; --attLeft; if (attLeft == 0) gain = peak; }
                else { gain *= decMul; if (--segLeft <= 0) { active = false; } }
                g = gain;
            }
            else if (mode == 1) // montée exponentielle puis coupe linéaire
            {
                if (riseLeft > 0) { gain *= riseMul; --riseLeft; g = gain; }
                else { g = juce::jmax (0.0f, gain - linStep * (float) (-riseLeft)); --riseLeft; if (g <= 0 || -riseLeft > (int) (0.06f * E.sr)) active = false; }
            }
            else // mode 2 : riser tenu puis relâché
            {
                if (! released) { if (riseLeft > 0) { gain *= riseMul; --riseLeft; } g = gain; }
                else { gain *= relCoef; g = gain; if (gain < 0.0003f) active = false; }
            }
            const float y = x * g;
            oL += y * panL * dryAmt; oR += y * panR * dryAmt;
        }
    };

    // ---------------- événements ----------------
    void handle (const Event& e)
    {
        switch (e.type)
        {
            case 1: startNote (e); break;
            case 2: glitchEvent (e); break;
            case 3: startRiser(); break;
            case 4: stopRiser(); break;
            case 5: doDrop (e.a); break;
            default: break;
        }
    }

    void startNote (const Event& e)
    {
        NoteVoice* v = nullptr;
        for (auto& n : notes) if (! n.active) { v = &n; break; }
        if (v == nullptr) v = &notes[(size_t) (rng.next() % notes.size())];
        const float vel = e.b, len = e.d; const bool acc = e.c > 0.5f; const int delaySamples = (int) (e.e * sr);
        const float jit = params.n[0].load() / 100.0f * 12.0f;
        float f = 440.0f * std::pow (2.0f, (e.a + params.pitch.load() + rng.bi() * jit - 69.0f) / 12.0f);
        f = juce::jlimit (110.0f, 9000.0f, f);
        const float det = 1.0f + rng.bi() * params.n[3].load() / 100.0f * 0.02f;
        const float spread = params.n[4].load() / 100.0f;
        const float pan = rng.bi() * juce::jmin (1.0f, 0.6f + spread * 0.4f);
        const float olap = 0.4f + params.olap.load() / 100.0f * 1.6f;
        const float dec = (0.07f + juce::jmin (0.5f, len * 0.45f) + (acc ? 0.12f : 0.0f)) * olap;
        const float midG = std::pow (params.curve.load() / 100.0f, 2.2f);
        const float glide = params.slw.load() / 100.0f;
        const float mix = params.pulse.load() / 100.0f;
        *v = NoteVoice();
        v->active = true; v->delay = delaySamples; v->vel = vel; v->acc = acc;
        const float ang = (pan + 1.0f) * kPi / 4.0f; v->panL = std::cos (ang); v->panR = std::sin (ang);
        const float fend = f * det;
        const float f0 = fend * std::pow (2.0f, (rng.uni() - 0.5f) * glide * 0.9f);
        v->f = fend; v->fcur = f0;
        const float sweepT = 0.008f + glide * 0.12f; const int sw = juce::jmax (1, (int) (sweepT * sr));
        v->fsweepLeft = (float) sw; v->fmul = std::pow (fend / f0, 1.0f / (float) sw);
        auto setEnv = [&] (Env& en, float peak, float d)
        {
            en.sr = sr; en.clear(); en.add (juce::jmax (0.0002f, peak), 0.002f);
            en.add (juce::jmax (0.0002f, peak * (0.05f + midG * 0.95f)), juce::jmax (0.001f, d * 0.4f - 0.002f));
            en.add (0.0001f, juce::jmax (0.001f, d * 0.6f)); en.begin();
        };
        setEnv (v->envS, vel * 0.16f * (1.0f - mix * 0.7f), dec);
        setEnv (v->envH, vel * 0.05f, dec * 0.35f);
        // l'harmonique 2.76 suit la fréquence de base : ph2 avance à dp*2.76
        if (acc) setEnv (v->envA, vel * 0.06f, dec * 0.8f);
        if (mix > 0.02f)
        {
            v->hasPulse = true;
            const float wRand = params.n[1].load() / 100.0f;
            v->pw = juce::jlimit (0.03f, 0.5f, params.pulseW.load() / 100.0f * 0.5f + (rng.uni() - 0.5f) * wRand * 0.4f);
            setEnv (v->envP, vel * 0.13f * mix, dec * 1.1f);
            setEnv (v->envP2, vel * 0.05f * mix, dec);
            const float res = params.res.load() / 100.0f;
            v->bp.set (juce::jmin (12000.0f, f * (2.0f + res * 3.0f)), 0.8f + res * 14.0f, sr);
            v->lp.set (juce::jmin (14000.0f, f * 10.0f), 0.7071f, sr);
        }
        const float click = params.n[2].load() / 100.0f;
        if (click > 0.02f) { v->clickAmp = click * vel * 0.25f; v->clickLeft = (int) (0.01f * sr); v->hpClick.set (2000.0f, 0.7071f, sr); }
    }

    SfxVoice* allocSfx()
    {
        for (auto& v : sfx) if (! v.active) return &v;
        return nullptr;
    }
    // sfx générique : osc, f0, f1, durée, filtre (type c0 c1 Q), pic, mode, pan, delay (s)
    SfxVoice* mkSfx (int osc, float f0, float f1, float d, int ftype, float c0, float c1, float Q, float peak, int mode, float pan, float delayS)
    {
        auto* v = allocSfx(); if (v == nullptr) return nullptr;
        *v = SfxVoice();
        v->active = true; v->osc = osc; v->f = f0; v->ftype = ftype; v->c = c0; v->Q = Q; v->peak = peak; v->mode = mode; v->delay = (int) (delayS * sr);
        const int n = juce::jmax (1, (int) (d * sr));
        if (osc != 3 && f1 > 0) { v->fleft = (float) n; v->fmul = std::pow (f1 / f0, 1.0f / (float) n); }
        if (ftype != 0) { v->cleft = (float) n; v->cmul = std::pow (c1 / c0, 1.0f / (float) n); v->filt.set (c0, Q, sr); }
        const float ang = (pan + 1.0f) * kPi / 4.0f; v->panL = std::cos (ang); v->panR = std::sin (ang);
        if (mode == 0) { v->gain = 0.0001f; v->attLeft = juce::jmax (1, (int) (0.002f * sr)); v->attMul = std::pow (peak / 0.0001f, 1.0f / (float) v->attLeft);
                         v->segLeft = n; v->decMul = std::pow (0.0001f / peak, 1.0f / (float) n); }
        else if (mode == 1) { v->gain = 0.0001f; v->riseLeft = juce::jmax (1, (int) (d * 0.95f * sr)); v->riseMul = std::pow (peak / 0.0001f, 1.0f / (float) v->riseLeft); v->linStep = peak / (0.05f * sr); }
        else { v->gain = 0.0001f; v->riseLeft = (int) (5.0f * sr); v->riseMul = std::pow (peak / 0.0001f, 1.0f / (float) v->riseLeft); v->relCoef = std::exp (-1.0f / (0.012f * sr)); }
        v->ph = rng.uni();
        return v;
    }

    float pickf (std::initializer_list<float> l) { return *(l.begin() + (rng.next() % l.size())); }

    void sfxZap (float w, float delayS)
    {
        const float f0 = 1800.0f + rng.uni() * 3500.0f, f1 = 140.0f + rng.uni() * 300.0f, d = 0.12f + rng.uni() * 0.25f;
        mkSfx (rng.uni() < 0.5f ? 1 : 2, f0, f1, d, 1, 9000.0f, 500.0f, 6.0f, 0.14f * w, 0, rng.bi(), delayS);
    }
    void sfxRise (float w, float delayS)
    {
        const float d = 0.35f + rng.uni() * 0.9f; const bool up = rng.uni() < 0.7f;
        mkSfx (3, 0, 0, d, 2, up ? 400.0f : 9000.0f, up ? 9000.0f : 300.0f, 3.0f + rng.uni() * 6.0f, (up ? 0.35f : 0.3f) * w, up ? 1 : 0, 0.0f, delayS);
    }
    void sfxScatter (float w, float delayS)
    {
        const int n = 14 + (int) (rng.uni() * 22.0f); const float span = 0.18f + rng.uni() * 0.35f;
        for (int i = 0; i < n; ++i)
        {
            const float f = 500.0f * std::pow (2.0f, rng.uni() * 4.3f), d = 0.006f + rng.uni() * 0.028f;
            mkSfx (rng.uni() < 0.5f ? 0 : 2, f, 0, d, 0, 0, 0, 1, 0.07f * w, 0, rng.bi(), delayS + rng.uni() * span);
        }
    }

    void glitchEvent (const Event& e)
    {
        const float strength = e.a, w = juce::jmax (0.05f, e.b * strength);
        const int mask = e.mask; const float p = juce::jmin (0.9f, 0.3f + w * 0.5f);
        gw = w;
        if (mask & 1) { ringF = pickf ({ 55, 110, 220, 330, 440, 880, 1320, 2500 }); ringT = rng.uni() < p ? 0.6f * w : 0.0f; } else ringT = 0;
        if (mask & 2) { crushBits = pickf ({ 3, 4, 5, 6, 8 }); crushT = rng.uni() < p ? 0.8f * w : 0.0f; } else crushT = 0;
        if (mask & 4) { filtF = 250.0f * std::pow (2.0f, rng.uni() * 5.0f); filtQ = 3.0f + rng.uni() * 14.0f; filtT = rng.uni() < p ? 0.7f * w : 0.0f; bpFx.set (filtF, filtQ, sr); } else filtT = 0;
        if ((mask & 8) && rng.uni() < p * 0.8f)
        {
            const float len = pickf ({ 0.045f, 0.06f, 0.09f, 0.125f, 0.17f, 0.25f });
            loopLen = juce::jlimit (16, (int) loopBuf[0].size() - 2, (int) (len * sr));
            loopRec = loopLen; loopFb = 0.97f; loopFbDecayAt = (int) ((len + 0.35f + rng.uni() * 0.9f) * sr); loopFbCount = 0;
            loopOutG = 0.9f * juce::jmin (1.0f, w + 0.3f); loopPlaying = true; loopRPos = 0; loopW = 0;
        }
        if ((mask & 16) && rng.uni() < p * 0.55f) sfxZap (w, rng.uni() * 0.05f);
        if ((mask & 32) && rng.uni() < p * 0.3f) sfxRise (w, 0.0f);
        if ((mask & 64) && rng.uni() < p * 0.5f) sfxScatter (w, 0.0f);
    }

    void startRiser()
    {
        stopRiser();
        riserA = mkSfx (3, 0, 0, 5.0f, 2, 380.0f, 9000.0f, 3.5f, 0.2f, 2, 0.0f, 0.0f);
        riserB = mkSfx (1, 230.0f, 2000.0f, 5.0f, 1, 700.0f, 8000.0f, 0.7f, 0.07f, 2, 0.0f, 0.0f);
    }
    void stopRiser()
    {
        for (auto* v : { riserA, riserB }) if (v != nullptr && v->active && v->mode == 2) v->released = true;
        riserA = riserB = nullptr;
    }
    void doDrop (float s)
    {
        const float k = 0.45f + s * 0.55f, d0 = 0.085f;
        mkSfx (3, 0, 0, 0.55f, 2, 1600.0f, 1600.0f, 0.7f, 0.55f * k, 0, 0.0f, d0);
        mkSfx (3, 0, 0, 0.18f, 3, 7000.0f, 7000.0f, 0.8f, 0.3f * k, 0, 0.0f, d0);
        mkSfx (3, 0, 0, 0.30f, 2, 500.0f, 500.0f, 1.2f, 0.3f * k, 0, 0.0f, d0);
        mkSfx (0, 1100.0f, 190.0f, 0.28f, 0, 0, 0, 1, 0.32f * k, 0, 0.0f, d0);
        sfxScatter (1.1f * k, d0 + 0.02f); sfxScatter (0.8f * k, d0 + 0.12f);
    }

    // glitch bus : ring / crush / filtre / stutter (mélangés avec le signal sec)
    void tickGlitch (float& mL, float& mR)
    {
        const float tau = 1.0f - std::exp (-1.0f / (0.004f * sr));
        ringG += (ringT - ringG) * tau; crushG += (crushT - crushG) * tau; filtG += (filtT - filtG) * tau;
        const float inL = mL, inR = mR;
        float addL = 0, addR = 0;
        if (ringG > 0.0005f)
        {
            ringPh += ringF / sr; ringPh -= std::floor (ringPh);
            const float m = std::sin (2 * kPi * ringPh);
            addL += inL * m * ringG; addR += inR * m * ringG;
        }
        if (crushG > 0.0005f)
        {
            const float L = std::pow (2.0f, crushBits);
            auto q = [&] (float x) { x = juce::jlimit (-1.0f, 1.0f, x * 1.3f); return std::round (x * L) / L; };
            addL += q (inL) * crushG; addR += q (inR) * crushG;
        }
        if (filtG > 0.0005f)
        {
            float l1, b1, h1; bpFx.process ((inL + inR) * 0.5f * 2.2f, l1, b1, h1);
            addL += b1 * filtG; addR += b1 * filtG;
        }
        if (loopPlaying)
        {
            // enregistre pendant loopRec échantillons puis boucle avec feedback
            const int sz = (int) loopBuf[0].size();
            float oL = 0, oR = 0;
            if (loopRec > 0) { loopBuf[0][(size_t) loopW] = inL; loopBuf[1][(size_t) loopW] = inR; loopW = (loopW + 1) % sz; --loopRec; oL = inL * 0; oR = inR * 0; }
            else
            {
                const int rp = ((loopW - loopLen) % sz + sz) % sz;
                oL = loopBuf[0][(size_t) rp]; oR = loopBuf[1][(size_t) rp];
                loopBuf[0][(size_t) loopW] = oL * loopFb; loopBuf[1][(size_t) loopW] = oR * loopFb; loopW = (loopW + 1) % sz;
                ++loopFbCount; if (loopFbCount > loopFbDecayAt) loopFb *= 0.9994f;
                if (loopFb < 0.003f) loopPlaying = false;
            }
            addL += oL * loopOutG; addR += oR * loopOutG;
        }
        mL = inL + addL; mR = inR + addR;
    }

    float sr = 48000.0f;
    juce::SpinLock lock;
    std::vector<Event> queue, local;
    std::array<NoteVoice, 128> notes;
    std::array<SfxVoice, 384> sfx;
    SfxVoice *riserA = nullptr, *riserB = nullptr;
    Rng rng;
    std::vector<float> fbBuf[2], loopBuf[2];
    int fbW = 0, loopW = 0, loopLen = 4000, loopRec = 0, loopFbDecayAt = 0, loopFbCount = 0, loopRPos = 0;
    bool loopPlaying = false;
    float loopFb = 0, loopOutG = 0;
    SVF fbLpL, fbLpR, hpL, hpR, bpFx;
    juce::Reverb reverb;
    float compEnv = 0, ringPh = 0, ringF = 220, ringT = 0, ringG = 0, crushBits = 5, crushT = 0, crushG = 0, filtF = 1500, filtQ = 6, filtT = 0, filtG = 0, gw = 0, held = 0;
};
} // namespace wings
