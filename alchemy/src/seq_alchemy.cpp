/*
 * seq-alchemy on the Hermetic Modular Alchemy Lab V2 -- two generated voices
 * (bass, lead) that follow a chord progression, four drum triggers, played
 * from the panel and, in Launchpad mode, from a Launchpad Mini MK3, a
 * Launch Control XL and a Haute42 gamepad on the front USB-C (through a
 * powered hub; see launchpad.h / usbh_hub_midi.h).
 *
 * The engine is core/seq.c (HAL-free, native tests in test/); this file is
 * the panel, the jacks and the controllers.
 *
 *   BASS / LEAD  DENSITY · LENGTH · SLIDE · JUMP · TONES · OCTAVE   (B1 cycles)
 *   DRUMS        KICK · SNARE · HAT · PERC · STYLE · PATTERN            (B1 cycles)
 *   SETUP        TEMPO · SWING · GLIDE · KEY · SCALE · CLOCK IN      (hold B3)
 *   B2           tap = play / stop
 *   Settings (B2+B3 2 s): brightness, presets, USB port, chord out + its
 *                MIDI channel; page 1 = SD picker
 *
 * Jacks (all +-5 V; pitch 1 V/oct, gates and triggers 0/5 V):
 *
 *   J1 clock in   J2 reset in
 *   J3 bass pitch J4 lead pitch  J5 bass gate  J6 lead gate     (MCP4728)
 *   J7 kick       J8 snare                                      (STM DAC)
 *   J9 hat        J10 perc                                      (codec DC)
 *
 * Pitch and gate of a voice latch in the same MCP write, so a gate never
 * rises before its pitch has moved.
 *
 * Chord out (Settings, default Off): the chord playing now, for Belt on a
 * second Lab to harmonize to, as MIDI on the rear header and/or as one CV
 * on J10. The choices:
 *   Off
 *   MIDI           the chord's notes, USART1 TX = header pin 8, 31,250 baud
 *   MIDI + clock   plus 0xF8 at 24 PPQN and start / stop
 *   CV             J10 = the chord's ROOT, 1 V/oct, 0 V = C, 0 .. 11/12 V;
 *                  Belt builds the chord in ITS key and scale, so set both
 *                  modules to the same KEY and SCALE. Changes on the bar,
 *                  holds when stopped. J10 is then not Perc: perc is silent.
 *   CV + MIDI      both (MIDI with clock)
 *   CV ref         J10 holds 0 V and nothing else is sent: pick this while
 *                  Belt runs its Learn, then go back to CV.
 * MIDI cable: pin 8 to the other Lab's pin 7, pin 7 to its pin 8, plus
 * grounds; NEVER a straight ribbon, the header carries +-12 V. CV cable: J10
 * to Belt's chord input, an ordinary patch cable. See midi_out.h and
 * core/chord_midi.h.
 *
 * The voice pots edit the CURRENT pattern slot's knobs. Each slot keeps its
 * own knobs, so when a queued slot takes over (or the Launchpad, the XL or
 * the gamepad changes a knob) the new values are pushed into the pager and
 * the pot has to catch them before it takes over again -- no jumps.
 *
 * Drums: programmed steps (Launchpad, or /seq/drums.txt on the card, read
 * at boot when it has changed) plus a generator on the DRUMS page -- the
 * four amounts are density thresholds (GEN 1-4) or Euclidean hit counts
 * (EUCLID 1-4) -- whose hits add to the programmed ones.
 *
 * Saving: the working state autosaves into preset slot kHomeSlot a few
 * seconds after the last change, but ONLY WHILE STOPPED -- a save erases
 * QSPI from the control thread and pauses the 1 ms poll for a few hundred
 * ms, which would drop beats. Stop the transport to keep what you made.
 *
 * Shared by copy with smack/mark/belt-alchemy and relay: the shape of this
 * file, the picker, the .lds, the autosave, the USB-role plumbing.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "daisy_seed.h"

#include "ff.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/crc32.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "chord_midi.h"
#include "extras.h"
#include "launchpad.h"
#include "midi_out.h"
#include "picker.h"
#include "seq.h"
#include "usb_shared.h"

#ifndef SEQ_VERSION
#define SEQ_VERSION "0.0.0"
#endif
#ifndef SEQ_GIT_HASH
#define SEQ_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

static constexpr uint32_t kBlockSize = 24u;

enum : uint8_t { kPageBass = 0, kPageLead = 1, kPageDrums = 2, kPageSetup = 3, kNumAppPages = 4 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab hw;
/* 19 KB: DTCM, where .bss lives on a bootloader build, is nearly full and
 * holds the stack, so the engine sits in AXI SRAM. NOLOAD there, which is
 * fine: seq_init() clears it before anything reads it. */
static seq_t S __attribute__((section(".axi_bss")));
static bool       g_lp_mode = false;     /* front port hosts the controllers */

/* ---- colours ------------------------------------------------------------- */

static constexpr LedPanel::Rgb kColBass   = {0xFF, 0x70, 0x10};
static constexpr LedPanel::Rgb kColLead   = {0x20, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColDrums  = {0xFF, 0x30, 0x50};
static constexpr LedPanel::Rgb kColKick   = {0xFF, 0x30, 0x30};
static constexpr LedPanel::Rgb kColSnare  = {0xFF, 0xD0, 0x20};
static constexpr LedPanel::Rgb kColHat    = {0x30, 0xE0, 0xFF};
static constexpr LedPanel::Rgb kColPerc   = {0xFF, 0x40, 0xC0};
static constexpr LedPanel::Rgb kColStyle  = {0xC0, 0xFF, 0x40};
static constexpr LedPanel::Rgb kColPat    = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColSetup  = {0x30, 0x10, 0x40};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColTempo  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColSwing  = {0xFF, 0xC0, 0x40};
static constexpr LedPanel::Rgb kColGlide  = {0x40, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColKey    = {0xC0, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColScale  = {0xFF, 0x60, 0xC0};
static constexpr LedPanel::Rgb kColClock  = {0x40, 0x80, 0xFF};
static constexpr LedPanel::Rgb kGreen     = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kBlue      = {0x00, 0x50, 0xFF};
static constexpr LedPanel::Rgb kMagenta   = {0xFF, 0x00, 0xA0};
static constexpr LedPanel::Rgb kIdle      = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kRedFlash  = {0xFF, 0x00, 0x00};

/* ---- labels ---------------------------------------------------------------- */

static const char* const kOctLabels[7]   = {"-3", "-2", "-1", "0", "+1", "+2", "+3"};
static const char* const kKeyLabels[12]  = {"C", "C#", "D", "D#", "E", "F",
                                            "F#", "G", "G#", "A", "A#", "B"};
static const char* const kScaleLabels[SEQ_SCALE_COUNT] = {
    "Major", "Minor", "Dorian", "Phrygian", "Lydian",
    "Mixolydian", "Harm minor", "Minor pent", "Major pent"};
static const char* const kClockLabels[4] = {"1 ppqn", "2 ppqn", "16ths", "24 ppqn"};
static const uint8_t     kClockPpqn[4]   = {1, 2, 4, 24};
static const char* const kUsbLabels[2]   = {"Mac", "Launchpad"};
static const char* const kChordOutLabels[CHORD_OUT_CHOICES] = {
    "Off", "MIDI", "MIDI + clock", "CV", "CV + MIDI", "CV ref"};
static const char* const kChannelLabels[16] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};
static const char* const kStyleLabels[SEQ_DSTYLE_COUNT] = {
    "Gen 1", "Gen 2", "Gen 3", "Gen 4", "Euclid 1", "Euclid 2", "Euclid 3", "Euclid 4"};
static const char* const kPatLabels[SEQ_DRUM_SLOTS] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};

/* ---- pages --------------------------------------------------------------- */

/* One page per voice. Panel positions, front view:
 *      [P1] B1 [P2]        DENSITY, LENGTH
 *      [P3] B2 [P4]        SLIDE,   JUMP
 *      [P5] B3 [P6]        TONES,   OCTAVE                                  */
#define VOICE_KNOBS(pfx, id, col)                                                   \
    static VirtualKnob pfx##_dens = VirtualKnob(kPotTopLeft, "Density")             \
        .Ident(id ".density").Ring(Level(col));                                     \
    static VirtualKnob pfx##_len = VirtualKnob(kPotTopRight, "Length")              \
        .Ident(id ".length").Ring(Level(col));                                      \
    static VirtualKnob pfx##_slide = VirtualKnob(kPotMiddleLeft, "Slide")           \
        .Ident(id ".slide").Ring(Level(col));                                       \
    static VirtualKnob pfx##_jump = VirtualKnob(kPotMiddleRight, "Jump")            \
        .Ident(id ".jump").Ring(Level(col));                                        \
    static VirtualKnob pfx##_tones = VirtualKnob(kPotBottomLeft, "Tones")           \
        .Ident(id ".tones").Ring(Level(col));                                       \
    static VirtualKnob pfx##_oct = VirtualKnob(kPotBottomRight, "Octave")           \
        .Selector(7).Labels(kOctLabels).Ident(id ".octave")                         \
        .Ring(SelectorRing(col, kColDim, 7));

VOICE_KNOBS(kb, "bass", kColBass)
VOICE_KNOBS(kl, "lead", kColLead)

static VirtualKnob* const kVoiceKnob[SEQ_VOICES][6] = {
    {&kb_dens, &kb_len, &kb_slide, &kb_jump, &kb_tones, &kb_oct},
    {&kl_dens, &kl_len, &kl_slide, &kl_jump, &kl_tones, &kl_oct},
};
static const uint8_t kVoicePage[SEQ_VOICES] = {kPageBass, kPageLead};

/* DRUMS: the current drum pattern's generator, and which pattern plays.
 *      [P1] B1 [P2]        KICK,  SNARE
 *      [P3] B2 [P4]        HAT,   PERC
 *      [P5] B3 [P6]        STYLE, PATTERN                                   */
static VirtualKnob kd_kick = VirtualKnob(kPotTopLeft, "Kick")
    .Ident("drums.kick").Ring(Level(kColKick));
static VirtualKnob kd_snare = VirtualKnob(kPotTopRight, "Snare")
    .Ident("drums.snare").Ring(Level(kColSnare));
static VirtualKnob kd_hat = VirtualKnob(kPotMiddleLeft, "Hat")
    .Ident("drums.hat").Ring(Level(kColHat));
static VirtualKnob kd_perc = VirtualKnob(kPotMiddleRight, "Perc")
    .Ident("drums.perc").Ring(Level(kColPerc));
static VirtualKnob kd_style = VirtualKnob(kPotBottomLeft, "Style")
    .Selector(SEQ_DSTYLE_COUNT).Labels(kStyleLabels).Ident("drums.style")
    .Ring(SelectorRing(kColStyle, kColDim, SEQ_DSTYLE_COUNT));
static VirtualKnob kd_pat = VirtualKnob(kPotBottomRight, "Pattern")
    .Selector(SEQ_DRUM_SLOTS).Labels(kPatLabels).Ident("drums.pattern")
    .Ring(SelectorRing(kColPat, kColDim, SEQ_DRUM_SLOTS));
static VirtualKnob* const kDrumKnob[6] = {&kd_kick, &kd_snare, &kd_hat, &kd_perc, &kd_style, &kd_pat};

/* SETUP: hold B3. Global, so these are plain pager values. */
static VirtualKnob k_tempo = VirtualKnob(kPotTopLeft, "Tempo")
    .Linear(30.f, 300.f).Ident("tempo").Unit("BPM").Ring(Level(kColTempo));
static VirtualKnob k_swing = VirtualKnob(kPotTopRight, "Swing")
    .Linear(0.5f, 0.75f).Ident("swing").Ring(Level(kColSwing));
static VirtualKnob k_glide = VirtualKnob(kPotMiddleLeft, "Glide")
    .Exp(1.f, 1000.f).Ident("glide").Unit("ms").Ring(Level(kColGlide));
static VirtualKnob k_key = VirtualKnob(kPotMiddleRight, "Key")
    .Selector(12).Labels(kKeyLabels).Ident("key")
    .Ring(SelectorRing(kColKey, kColDim, 12));
static VirtualKnob k_scale = VirtualKnob(kPotBottomLeft, "Scale")
    .Selector(SEQ_SCALE_COUNT).Labels(kScaleLabels).Ident("scale")
    .Ring(SelectorRing(kColScale, kColDim, SEQ_SCALE_COUNT));
static VirtualKnob k_clock = VirtualKnob(kPotBottomRight, "Clock in")
    .Selector(4).Labels(kClockLabels).Ident("clock")
    .Ring(SelectorRing(kColClock, kColDim, 4));

static Page bass_page = Page(kPageBass).Name("Bass").Color("#ff7010")
    .Knobs(kb_dens, kb_len, kb_slide, kb_jump, kb_tones, kb_oct);
static Page lead_page = Page(kPageLead).Name("Lead").Color("#20c0ff")
    .Knobs(kl_dens, kl_len, kl_slide, kl_jump, kl_tones, kl_oct);
static Page drums_page = Page(kPageDrums).Name("Drums").Color("#ff3050")
    .Knobs(kd_kick, kd_snare, kd_hat, kd_perc, kd_style, kd_pat);
static Page setup_page = Page(kPageSetup).Name("Setup").Color("#c060ff")
    .Knobs(k_tempo, k_swing, k_glide, k_key, k_scale, k_clock);

/* ---- descriptor metadata ------------------------------------------------- */

static Jack jk_j1  ("J1",  "Clock in",   JackSig::Trig);
static Jack jk_j2  ("J2",  "Reset in",   JackSig::Trig);
static Jack jk_j3  ("J3",  "Bass pitch", JackSig::CvBi);
static Jack jk_j4  ("J4",  "Lead pitch", JackSig::CvBi);
static Jack jk_j5  ("J5",  "Bass gate",  JackSig::Trig);
static Jack jk_j6  ("J6",  "Lead gate",  JackSig::Trig);
static Jack jk_j7  ("J7",  "Kick",       JackSig::Trig);
static Jack jk_j8  ("J8",  "Snare",      JackSig::Trig);
static Jack jk_j9  ("J9",  "Hat",        JackSig::Trig);
static Jack jk_j10 ("J10", "Perc",       JackSig::Trig);

static VirtualButton bt_voice = VirtualButton("b1", "Bass / Lead / Drums")
    .Action("Tap", "Cycle the pots: bass, lead, drums");
static VirtualButton bt_play = VirtualButton("b2", "Play / Stop")
    .Action("Tap", "Start from the top, or stop (stopping also lets it save)");
static VirtualButton bt_setup = VirtualButton("b3", "Setup")
    .Action("Hold", "Show the Setup page");

static Manual manual = Manual()
    .Tagline("Generative bass + lead over a chord progression, four drum triggers")
    .Preamble(
        "**seq** plays two generated voices -- bass and lead, pitch and gate "
        "each -- that follow a chord progression, and four drum triggers. "
        "J3/J4 are the bass and lead pitch (1 V/oct), J5/J6 their gates, "
        "J7-J10 kick, snare, hat and perc. J1 takes a clock (it takes over "
        "from the internal tempo on the first pulse; two seconds without one "
        "stops the transport) and J2 a reset. B1 cycles the pots through the "
        "bass and the lead -- DENSITY, LENGTH, SLIDE, JUMP, TONES (chord "
        "tones only .. any scale tone) and OCTAVE -- and the drums: an "
        "amount per drum, STYLE (Gen 1-4 thresholds, Euclid 1-4) and "
        "PATTERN. Drum steps come from the Launchpad or from /seq/drums.txt "
        "on the card (read at power-up when it has changed). Hold B3 for TEMPO, SWING, GLIDE, "
        "KEY, SCALE and the clock-in rate. B2 plays and stops. The working "
        "state saves itself a few seconds after you stop. In Launchpad mode "
        "(Settings: USB port) a Launchpad Mini MK3, a Launch Control XL and "
        "a Haute42 gamepad play it through a powered hub; this page is only "
        "reachable in Mac mode.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Cycle(hw.buttons[kButtonB1], kPageBass, kPageLead, kPageDrums)
                               .Shift(hw.buttons[kButtonB3], kPageSetup);

/*
 * This firmware's own preset slot. Every Alchemy firmware in the family
 * keeps its working state in a slot of its own -- seq 5, Meld 6, Warps 7,
 * Clouds 8, Elements 9, Marbles 10, Plaits 11, Smack 12, Mark 13,
 * Belt 14, Relay 15 -- so switching firmware through the SD picker does not
 * reset the one you left. Slots 0-4 are for presets saved by hand. (seq was
 * on 11 until 2026-10-02, which is Plaits' -- the two would have wiped each
 * other's state on a shared card.)
 */
static constexpr uint8_t kHomeSlot = 5u;
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, &pager);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "seq_alchemy", "seq", SEQ_VERSION, SEQ_GIT_HASH);
static SeqExtras      extras;
static SelectorHandle s_usb;
static SelectorHandle s_chord_out;
static SelectorHandle s_chord_ch;

/* Chord out: the byte ring lives in DTCM with the rest of the poll's state
 * (~300 B). */
static chord_midi_t      g_cm;
static chord_cv_t        g_cv;
static uint8_t           g_cv_mode = CHORD_CV_OFF;
static chord_midi_ring_t g_cm_ring;

/* ---- selector hysteresis (see relay_alchemy.cpp) ------------------------- */

#define HYST 0.02f

struct Zoned
{
    int   zones;
    int   last      = -1;
    float last_norm = 0.0f;
};

static int zoned(Zoned& z, float norm)
{
    if(norm < 0.0f) norm = 0.0f;
    if(norm > 1.0f) norm = 1.0f;
    int v = (int)(norm * (float)z.zones);
    if(v >= z.zones) v = z.zones - 1;
    if(z.last >= 0 && v != z.last && fabsf(norm - z.last_norm) < HYST) return z.last;
    if(v != z.last)
    {
        z.last      = v;
        z.last_norm = norm;
    }
    return v;
}

/* ---- voice pots <-> the current slot's knobs ------------------------------ */

static float g_phys[kNumPots];            /* physical pot positions, 0..1 */
static float g_applied[SEQ_VOICES][6];    /* last value either side agreed */
static Zoned z_oct[SEQ_VOICES] = {{7}, {7}};

/* The knob as a pot position. OCTAVE sits in the middle of its zone. */
static float knob_norm(const seq_knobs_t* k, int p)
{
    switch(p)
    {
        case 0: return k->density;
        case 1: return k->length;
        case 2: return k->slide;
        case 3: return k->jump;
        case 4: return k->tone_bias;
        default: return ((float)(k->octave + 3) + 0.5f) / 7.0f;
    }
}

static void set_knob(seq_knobs_t* k, int p, float n)
{
    switch(p)
    {
        case 0: k->density = n; break;
        case 1: k->length = n; break;
        case 2: k->slide = n; break;
        case 3: k->jump = n; break;
        case 4: k->tone_bias = n; break;
        default: break;
    }
}

/*
 * Two writers, one value. A pot that moved (and has caught) writes the
 * knob; a knob that changed under the pots (a slot switch, the Launchpad,
 * the XL, the gamepad) is written into the pager with catch re-armed, so
 * the pot has to come to it before it takes over again.
 */
static void sync_voice_knobs(void)
{
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        seq_knobs_t* k = seq_knobs(&S, v);
        for(int p = 0; p < 6; p++)
        {
            const float pn = kVoiceKnob[v][p]->Norm();
            if(p == 5)
            {
                /* OCTAVE: zones, with hysteresis */
                const int zone = zoned(z_oct[v], pn);
                const int applied_zone = (int)(g_applied[v][p] * 7.0f);
                if(zone != applied_zone)
                {
                    k->octave       = (int8_t)(zone - 3);
                    g_applied[v][p] = knob_norm(k, p);
                }
                else if(k->octave + 3 != applied_zone)
                {
                    const float sn = knob_norm(k, p);
                    pager.SetStored(kVoicePage[v], (uint8_t)p, sn, g_phys);
                    g_applied[v][p] = sn;
                    z_oct[v].last   = -1;
                }
                continue;
            }
            const float sn = knob_norm(k, p);
            if(fabsf(pn - g_applied[v][p]) > 1e-4f)
            {
                set_knob(k, p, pn);
                g_applied[v][p] = pn;
            }
            else if(fabsf(sn - g_applied[v][p]) > 1e-4f)
            {
                pager.SetStored(kVoicePage[v], (uint8_t)p, sn, g_phys);
                g_applied[v][p] = sn;
            }
        }
    }
}

/* ---- setup page -> the engine --------------------------------------------- */

static Zoned z_key{12}, z_scale{SEQ_SCALE_COUNT}, z_clock{4};

static void apply_setup(void)
{
    S.bpm      = k_tempo.Value();
    S.swing    = k_swing.Value();
    const float g = k_glide.Value();
    S.glide_ms = g <= 2.0f ? 0 : (uint16_t)g;
    S.prog.root  = (uint8_t)zoned(z_key, k_key.Norm());
    S.prog.scale = (uint8_t)zoned(z_scale, k_scale.Norm());
    S.clock_ppqn = kClockPpqn[zoned(z_clock, k_clock.Norm())];
}

/* ---- drum pots <-> the current drum pattern ------------------------------ */

/* Same two-writer rule as the voice pots. KICK..PERC and STYLE edit the
 * pattern playing now; PATTERN queues one for the next bar. */
static float g_dapplied[6];
static Zoned z_style{SEQ_DSTYLE_COUNT}, z_pat{SEQ_DRUM_SLOTS};

static float zone_norm(int z, int n) { return ((float)z + 0.5f) / (float)n; }

static void sync_drum_knobs(void)
{
    seq_drums_t* dr = seq_drums(&S);
    for(int p = 0; p < 6; p++)
    {
        const float pn = kDrumKnob[p]->Norm();
        if(p < 4)
        {
            const float sn = dr->gen[p];
            if(fabsf(pn - g_dapplied[p]) > 1e-4f)
            {
                dr->gen[p]    = pn;
                g_dapplied[p] = pn;
            }
            else if(fabsf(sn - g_dapplied[p]) > 1e-4f)
            {
                pager.SetStored(kPageDrums, (uint8_t)p, sn, g_phys);
                g_dapplied[p] = sn;
            }
            continue;
        }
        const int n       = p == 4 ? SEQ_DSTYLE_COUNT : SEQ_DRUM_SLOTS;
        Zoned&    z       = p == 4 ? z_style : z_pat;
        const int zone    = zoned(z, pn);
        const int applied = (int)(g_dapplied[p] * (float)n);
        const int now     = p == 4 ? dr->style : S.drum_queued;
        if(zone != applied)
        {
            if(p == 4) dr->style = (uint8_t)zone;
            else seq_drum_queue(&S, zone);
            g_dapplied[p] = zone_norm(zone, n);
        }
        else if(now != applied)
        {
            g_dapplied[p] = zone_norm(now, n);
            pager.SetStored(kPageDrums, (uint8_t)p, g_dapplied[p], g_phys);
            z.last = -1;
        }
    }
}

/* ---- /seq/drums.txt ------------------------------------------------------------ */

/* Read at power-up. Imported only when its CRC differs from the last one
 * imported (kept in the saved state), and the file is left on the card:
 * if the module is switched off before the import was saved, the next
 * power-up simply imports it again. B2 flashes green for an import, red
 * for a file that makes no sense (nothing changes then). */
alignas(32) static ALCHEMY_SDMMC_BSS FIL  s_drums_fil;
alignas(32) static ALCHEMY_SDMMC_BSS char s_drums_buf[8192];
static uint32_t g_import_flash_until = 0;
static bool     g_import_ok          = false;

static void import_drums_file(uint32_t now)
{
    if(!sd.EnsureMounted(now)) return;
    if(f_open(&s_drums_fil, "/seq/drums.txt", FA_READ) != FR_OK) return;
    UINT got = 0;
    const FRESULT r = f_read(&s_drums_fil, s_drums_buf, sizeof s_drums_buf, &got);
    f_close(&s_drums_fil);
    if(r != FR_OK || got == 0 || got >= sizeof s_drums_buf) return;
    const uint32_t crc = hostlink::Crc32(reinterpret_cast<const uint8_t*>(s_drums_buf), got);
    if(crc == S.drums_file_crc) return;
    const int n = seq_drums_parse(&S, s_drums_buf, got);
    g_import_ok          = n >= 0;
    g_import_flash_until = now + 1500u;
    if(n >= 0) S.drums_file_crc = crc;
}

/* ---- jacks ------------------------------------------------------------------ */

static float g_last_volts[kNumCvJacks];
static bool  g_first_write = true;

static float clamp_pitch(float v) { return v < 0.f ? 0.f : (v > 5.f ? 5.f : v); }

/* J9 / J10 are the codec's DC outputs, and the real transfer is INVERTING:
 * a +1 sample is -4 x 39k/18k = -8.667 V at the jack (Seed2 DFM datasheet
 * fig. 1.4; alchemy-sdk 4eb0e4c "Fix/codec cv voltage", v0.12.0). The SDK
 * pinned here (v0.11) still writes volts / 5 as the sample, which would put
 * -1.73 x the volts on the jack. So on that SDK the volts are pre-scaled to
 * land right; an SDK with the fix (v2_codec_cv.h) gets them as they are. */
#if __has_include("alchemy/hw/v2_codec_cv.h")
static float codec_volts(float v) { return v; }
#else
static float codec_volts(float v) { return v * kCodecJackFullScaleVolts / (-4.0f * 39.0f / 18.0f); }
#endif

/* Stage what changed on the MCP jacks and latch once; the STM DAC and codec
 * jacks are direct. A Flush with nothing staged returns at once. */
static void write_jacks(const seq_out_t& o)
{
    const float volts[kNumCvJacks] = {
        clamp_pitch(o.pitch[SEQ_BASS]), clamp_pitch(o.pitch[SEQ_LEAD]),
        o.gate[SEQ_BASS] ? 5.f : 0.f,   o.gate[SEQ_LEAD] ? 5.f : 0.f,
        o.trig[SEQ_KICK] ? 5.f : 0.f,   o.trig[SEQ_SNARE] ? 5.f : 0.f,
        codec_volts(o.trig[SEQ_HAT] ? 5.f : 0.f),
        /* J10: perc, or the chord CV (perc silent) when Chord out has CV */
        codec_volts(g_cv_mode != CHORD_CV_OFF ? chord_cv_update(&g_cv, &S, g_cv_mode)
                                              : o.trig[SEQ_PERC] ? 5.f : 0.f),
    };
    for(uint8_t j = 0; j < kNumCvJacks; j++)
    {
        if(!g_first_write && volts[j] == g_last_volts[j]) continue;
        g_last_volts[j] = volts[j];
        hw.cv_jacks[j].StageVolts(volts[j]);
    }
    g_first_write = false;
    hw.FlushCvOutputs();
}

/* ---- transport + mutes -------------------------------------------------------- */

static uint32_t g_clock_flash_until = 0;

static void toggle_play(void)
{
    if(S.running) seq_stop(&S);
    else seq_play(&S);
}

/* Mutes the player chose, and mutes held on the gamepad; the engine gets
 * the OR of the two. */
static bool     g_mute_v[SEQ_VOICES], g_mute_d[SEQ_DRUMS];
static uint32_t g_pad_held = 0;

static void apply_mutes(void);

/* ---- Launchpad Mini MK3 -------------------------------------------------------- *
 *
 * Top row: BASS · LEAD · DRUMS · PATTERNS · - · - · SHIFT · PLAY
 * Side column (every mode): kick, snare, hat, perc, bass, lead mutes.
 *
 * BASS / LEAD
 *   rows 1-4   the steps, 8 a row (32 shown; past 32 the page follows the
 *              playhead). Green note, amber slide, grey rest, red playhead.
 *              Tap a step: the loop ends there.
 *   row 7      the voice's 8 pattern slots: green playing, amber queued.
 *              Tap to queue for the next bar; hold one and tap another to
 *              copy the held slot into it.
 *   row 8      ROLL · MUTATE · UNDO · LOCK · OCT- · OCT+ · RATE · MUTE
 * DRUMS
 *   rows 1-2 kick, 3-4 snare, 5-6 hat, 7-8 perc: 16 steps each. Tap = on
 *   (100 %) / off; SHIFT+tap steps the chance down 100-75-50-25 %. Grey is
 *   a hit from the generator (DRUMS pot page); tapping it programs it.
 * PATTERNS
 *   row 1 bass slots, row 2 lead slots, rows 3-4 the 16 drum patterns.
 */
enum : uint8_t { kModeBass = 0, kModeLead = 1, kModeDrums = 2, kModePatterns = 3 };
static uint8_t g_lp_mode_sel = kModeBass;
static bool    g_lp_shift    = false;
static int8_t  g_slot_held   = -1;   /* voice-mode slot pad held, for copy */
static bool    g_slot_copied = false;
static uint8_t g_lp_stage    = 0;
static uint32_t g_lp_boot_ms = 0;

static const uint8_t kDrumCol[SEQ_DRUMS]    = {lp::kRed, lp::kAmber, lp::kCyan, lp::kMagenta};
static const uint8_t kDrumColDim[SEQ_DRUMS] = {lp::kRedDim, lp::kAmberDim, lp::kCyanDim,
                                               lp::kMagentaDim};

static void voice_action(int v, int x)
{
    seq_knobs_t* k = seq_knobs(&S, v);
    switch(x)
    {
        case 0: seq_roll(&S, v); break;
        case 1: seq_mutate(&S, v, 0.25f); break;
        case 2: seq_undo(&S, v); break;
        case 3: seq_lock(&S, v, !S.pat[v][S.v[v].slot].locked); break;
        case 4: if(k->octave > -3) k->octave--; break;
        case 5: if(k->octave < 3) k->octave++; break;
        case 6: k->div = k->div == 1 ? 2 : (k->div == 2 ? 4 : 1); break;
        case 7: g_mute_v[v] = !g_mute_v[v]; apply_mutes(); break;
        default: break;
    }
}

static int step_page_base(int v)
{
    const int steps = seq_knobs(&S, v)->steps;
    const int cur   = S.v[v].cur_step;
    if(steps <= 32 || cur < 0) return 0;
    return (cur % steps) / 32 * 32;
}

static void lp_grid(const lp::Event& e)
{
    const int x = e.x, y = e.y;
    if(g_lp_mode_sel == kModeBass || g_lp_mode_sel == kModeLead)
    {
        const int v = g_lp_mode_sel == kModeBass ? SEQ_BASS : SEQ_LEAD;
        if(y < 4)
        {
            if(!e.down) return;
            const int k = step_page_base(v) + y * 8 + x;
            if(k < SEQ_MAX_STEPS) seq_knobs(&S, v)->steps = (uint8_t)(k + 1);
        }
        else if(y == 6)
        {
            if(e.down)
            {
                if(g_slot_held >= 0 && g_slot_held != x)
                {
                    seq_voice_copy(&S, v, g_slot_held, x);
                    g_slot_copied = true;
                }
                else if(g_slot_held < 0)
                {
                    g_slot_held   = (int8_t)x;
                    g_slot_copied = false;
                }
            }
            else if(g_slot_held == x)
            {
                if(!g_slot_copied) seq_voice_queue(&S, v, x);
                g_slot_held = -1;
            }
        }
        else if(y == 7 && e.down)
            voice_action(v, x);
        return;
    }
    if(!e.down) return;
    if(g_lp_mode_sel == kModeDrums)
    {
        const int d = y / 2, k = (y % 2) * 8 + x;
        const int p = S.drums[S.drum_slot].prob[d][k];
        int np;
        if(g_lp_shift) np = p <= 1 ? 4 : p - 1;
        else np = p ? 0 : 4;
        seq_drum_set(&S, d, k, np);
        return;
    }
    /* patterns */
    if(y == 0) seq_voice_queue(&S, SEQ_BASS, x);
    else if(y == 1) seq_voice_queue(&S, SEQ_LEAD, x);
    else if(y == 2 || y == 3) seq_drum_queue(&S, (y - 2) * 8 + x);
}

static void lp_events(void)
{
    lp::Event e;
    const bool live = !settings.IsActive();
    while(lp::PopEvent(&e))
    {
        if(!live) continue;
        switch(e.kind)
        {
            case lp::Kind::Top:
                if(e.x == 6) g_lp_shift = e.down;
                else if(!e.down) break;
                else if(e.x <= kModePatterns)
                {
                    g_lp_mode_sel = e.x;
                    g_slot_held   = -1;
                }
                else if(e.x == 7) toggle_play();
                break;
            case lp::Kind::Side:
                if(!e.down) break;
                if(e.y < SEQ_DRUMS) g_mute_d[e.y] = !g_mute_d[e.y];
                else if(e.y < SEQ_DRUMS + SEQ_VOICES) g_mute_v[e.y - SEQ_DRUMS] = !g_mute_v[e.y - SEQ_DRUMS];
                apply_mutes();
                break;
            case lp::Kind::Grid: lp_grid(e); break;
        }
    }
}

static uint8_t slot_col(bool playing, bool queued, bool blink)
{
    if(queued) return blink ? lp::kAmber : lp::kAmberDim;
    return playing ? lp::kGreen : lp::kGrey;
}

static void lp_paint(uint32_t now)
{
    const bool blink = (now / 125u) & 1u;
    for(int x = 0; x < 8; x++)
    {
        uint8_t c = lp::kOff;
        if(x <= kModePatterns) c = x == g_lp_mode_sel ? lp::kWhite : lp::kGrey;
        if(x == 6) c = g_lp_shift ? lp::kWhite : lp::kGrey;
        if(x == 7) c = S.running ? lp::kGreen : lp::kGreenDim;
        lp::SetTop((uint8_t)x, c);
    }
    for(int d = 0; d < SEQ_DRUMS; d++)
        lp::SetSide((uint8_t)d, S.drum_mute[d] ? lp::kRed : kDrumColDim[d]);
    lp::SetSide(4, S.v[SEQ_BASS].mute ? lp::kRed : lp::kOrangeDim);
    lp::SetSide(5, S.v[SEQ_LEAD].mute ? lp::kRed : lp::kCyanDim);
    lp::SetSide(6, lp::kOff);
    lp::SetSide(7, lp::kOff);

    if(g_lp_mode_sel == kModeBass || g_lp_mode_sel == kModeLead)
    {
        const int v     = g_lp_mode_sel == kModeBass ? SEQ_BASS : SEQ_LEAD;
        const int steps = seq_knobs(&S, v)->steps;
        const int base  = step_page_base(v);
        const int cur   = S.running && S.v[v].cur_step >= 0 ? S.v[v].cur_step % steps : -1;
        for(int y = 0; y < 4; y++)
            for(int x = 0; x < 8; x++)
            {
                const int k = base + y * 8 + x;
                uint8_t c = lp::kOff;
                if(k < steps)
                {
                    const seq_step_t st = seq_step(&S, v, k);
                    c = !st.on ? lp::kGrey : (st.slide ? lp::kAmber : lp::kGreen);
                    if(k == cur) c = lp::kRed;
                }
                lp::SetGrid((uint8_t)x, (uint8_t)y, c);
            }
        for(int x = 0; x < 8; x++)
        {
            lp::SetGrid((uint8_t)x, 4, lp::kOff);
            lp::SetGrid((uint8_t)x, 5, lp::kOff);
            const bool held = g_slot_held == x;
            lp::SetGrid((uint8_t)x, 6,
                        held ? lp::kWhite
                             : slot_col(S.v[v].slot == x,
                                        S.v[v].queued == x && S.v[v].queued != S.v[v].slot,
                                        blink));
        }
        const seq_knobs_t* k = seq_knobs(&S, v);
        const bool locked = S.pat[v][S.v[v].slot].locked;
        const uint8_t row[8] = {
            lp::kBlue, lp::kCyan, lp::kMagentaDim,
            locked ? lp::kAmber : lp::kAmberDim,
            k->octave < 0 ? lp::kOrange : lp::kOrangeDim,
            k->octave > 0 ? lp::kOrange : lp::kOrangeDim,
            k->div == 1 ? lp::kGreenDim : lp::kGreen,
            S.v[v].mute ? lp::kRed : lp::kRedDim,
        };
        for(int x = 0; x < 8; x++) lp::SetGrid((uint8_t)x, 7, row[x]);
    }
    else if(g_lp_mode_sel == kModeDrums)
    {
        const seq_drums_t& dr = S.drums[S.drum_slot];
        for(int d = 0; d < SEQ_DRUMS; d++)
        {
            const int cur = S.running && S.drum_step[d] >= 0 ? S.drum_step[d] % dr.steps[d] : -1;
            for(int k = 0; k < 16; k++)
            {
                bool gen = false;
                const int p = seq_drum_prob(&S, d, k, &gen);
                uint8_t c = k >= dr.steps[d] ? lp::kOff
                          : (p == 0 ? lp::kOff
                                    : (gen ? lp::kGrey : (p >= 3 ? kDrumCol[d] : kDrumColDim[d])));
                if(k == cur) c = lp::kWhite;
                lp::SetGrid((uint8_t)(k % 8), (uint8_t)(d * 2 + k / 8), c);
            }
        }
    }
    else
    {
        for(int x = 0; x < 8; x++)
        {
            lp::SetGrid((uint8_t)x, 0, slot_col(S.v[SEQ_BASS].slot == x,
                        S.v[SEQ_BASS].queued == x && S.v[SEQ_BASS].queued != S.v[SEQ_BASS].slot, blink));
            lp::SetGrid((uint8_t)x, 1, slot_col(S.v[SEQ_LEAD].slot == x,
                        S.v[SEQ_LEAD].queued == x && S.v[SEQ_LEAD].queued != S.v[SEQ_LEAD].slot, blink));
            for(int r = 0; r < 2; r++)
            {
                const int sl = r * 8 + x;
                lp::SetGrid((uint8_t)x, (uint8_t)(2 + r),
                            slot_col(S.drum_slot == sl,
                                     S.drum_queued == sl && S.drum_queued != S.drum_slot, blink));
            }
            for(int y = 4; y < 8; y++) lp::SetGrid((uint8_t)x, (uint8_t)y, lp::kOff);
        }
    }
    lp::SetLogo(lp::Connected() ? lp::kGreen : lp::kOff);
}

/* ---- Launch Control XL --------------------------------------------------------- *
 *
 * Knob row 1 = bass, row 2 = lead: DENSITY · LENGTH · SLIDE · JUMP · TONES ·
 * OCTAVE · RANGE · SWING (how much of the global swing the voice takes).
 * Knob row 3 = drums: KICK · SNARE · HAT · PERC amounts · STYLE · PATTERN.
 * Buttons, upper row: bass ROLL · MUTATE · UNDO · LOCK, lead the same.
 * Lower row: mutes bass · lead · kick · snare · hat · perc, -, PLAY.
 */
static void xl_frame(void)
{
    if(!xl::Connected()) return;
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        seq_knobs_t* k = seq_knobs(&S, v);
        for(int c = 0; c < 8; c++)
        {
            uint8_t val;
            if(!xl::Knob((uint8_t)v, (uint8_t)c, &val)) continue;
            const float n = (float)val / 127.0f;
            if(c < 5) set_knob(k, c, n);
            else if(c == 5) k->octave = (int8_t)(lroundf(n * 6.0f) - 3);
            else if(c == 6) k->range = (uint8_t)(1 + lroundf(n * 2.0f));
            else k->swing = n;
        }
    }
    for(int c = 0; c < 6; c++)
    {
        uint8_t val;
        if(!xl::Knob(2, (uint8_t)c, &val)) continue;
        const float n = (float)val / 127.0f;
        if(c < 4) seq_drums(&S)->gen[c] = n;
        else if(c == 4) seq_drums(&S)->style = (uint8_t)(n * (SEQ_DSTYLE_COUNT - 0.01f));
        else seq_drum_queue(&S, (int)(n * (SEQ_DRUM_SLOTS - 0.01f)));
    }
    xl::Button b;
    while(xl::PopButton(&b))
    {
        if(!b.down || settings.IsActive()) continue;
        if(b.row == 0)
        {
            const int v = b.col / 4;
            voice_action(v, b.col % 4);
        }
        else if(b.col < 2) { g_mute_v[b.col] = !g_mute_v[b.col]; apply_mutes(); }
        else if(b.col < 6) { g_mute_d[b.col - 2] = !g_mute_d[b.col - 2]; apply_mutes(); }
        else if(b.col == 7) toggle_play();
    }
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        const bool locked = S.pat[v][S.v[v].slot].locked;
        xl::SetButtonLed(0, (uint8_t)(v * 4 + 0), xl::kGreenDim);
        xl::SetButtonLed(0, (uint8_t)(v * 4 + 1), xl::kGreenDim);
        xl::SetButtonLed(0, (uint8_t)(v * 4 + 2), xl::kAmberDim);
        xl::SetButtonLed(0, (uint8_t)(v * 4 + 3), locked ? xl::kAmber : xl::kAmberDim);
        xl::SetButtonLed(1, (uint8_t)v, S.v[v].mute ? xl::kRed : xl::kGreen);
    }
    for(int d = 0; d < SEQ_DRUMS; d++)
        xl::SetButtonLed(1, (uint8_t)(2 + d), S.drum_mute[d] ? xl::kRed : xl::kGreen);
    xl::SetButtonLed(1, 6, xl::kOff);
    xl::SetButtonLed(1, 7, S.running ? xl::kGreen : xl::kGreenDim);
}

/* ---- Haute42 (XInput, through the hub) ------------------------------------------ *
 *
 * Held:   A kick · B snare · X hat · Y perc · LB bass · RB lead   = mute while held
 *         LT / RT  bass / lead density to full while held (a fill)
 * Tapped: Start play/stop · Left/Right bass octave -/+ · Down/Up lead octave -/+
 *         L3 / R3 mutate bass / lead
 */
static float g_fill_saved[SEQ_VOICES] = {-1.f, -1.f};

static void pad_poll(void)
{
    const uint32_t b = settings.IsActive() ? 0u : pad::Buttons();
    const uint32_t pressed = b & ~g_pad_held;
    const uint32_t released = g_pad_held & ~b;
    g_pad_held = b;
    if(pressed || released) apply_mutes();

    if(pressed & pad::kStart) toggle_play();
    if(pressed & pad::kLeft) voice_action(SEQ_BASS, 4);
    if(pressed & pad::kRight) voice_action(SEQ_BASS, 5);
    if(pressed & pad::kDown) voice_action(SEQ_LEAD, 4);
    if(pressed & pad::kUp) voice_action(SEQ_LEAD, 5);
    if(pressed & pad::kL3) voice_action(SEQ_BASS, 1);
    if(pressed & pad::kR3) voice_action(SEQ_LEAD, 1);

    const uint32_t fill_bit[SEQ_VOICES] = {pad::kLT, pad::kRT};
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        seq_knobs_t* k = seq_knobs(&S, v);
        if(pressed & fill_bit[v])
        {
            g_fill_saved[v] = k->density;
            k->density = 1.0f;
        }
        if((released & fill_bit[v]) && g_fill_saved[v] >= 0.f)
        {
            k->density      = g_fill_saved[v];
            g_fill_saved[v] = -1.f;
        }
    }
}

static void apply_mutes(void)
{
    const uint32_t kPadDrum[SEQ_DRUMS]   = {pad::kA, pad::kB, pad::kX, pad::kY};
    const uint32_t kPadVoice[SEQ_VOICES] = {pad::kLB, pad::kRB};
    for(int d = 0; d < SEQ_DRUMS; d++)
        seq_drum_mute(&S, d, g_mute_d[d] || (g_pad_held & kPadDrum[d]));
    for(int v = 0; v < SEQ_VOICES; v++)
        seq_voice_mute(&S, v, g_mute_v[v] || (g_pad_held & kPadVoice[v]));
}

/* ---- buttons (1 ms poll) ------------------------------------------------------------- */

struct Press
{
    bool down = false;
    bool veto = false;   /* B3 was held during the press: release is inert */
};
static Press b2;

/* Buttons still down when Settings closed: ignored until they are let go
 * (the press that closes Settings must not also count here). */
static uint8_t g_btn_swallow = 0;

static bool btn_live(uint8_t b)
{
    const bool    down = hw.buttons[b].Pressed();
    const uint8_t bit  = (uint8_t)(1u << b);
    if(g_btn_swallow & bit)
    {
        if(down) return false;
        g_btn_swallow = (uint8_t)(g_btn_swallow & ~bit);
    }
    return down;
}

static void OnPoll(uint32_t now)
{
    /* Clock and reset first, then time, then the jacks. Edge times are the
     * poll's, so a clock edge lands within a millisecond. */
    const uint32_t us = System::GetUs();
    if(hw.j2.RisingEdge()) seq_reset_edge(&S, us);
    if(hw.j1.RisingEdge())
    {
        seq_clock_edge(&S, us);
        g_clock_flash_until = now + 30;
    }
    seq_out_t o;
    seq_process(&S, us, &o);
    /* Chord out: the choice sets the MIDI and the J10 CV modes; the CV is
     * written with the jacks. For the MIDI, what changed goes into the ring
     * and the ring into the UART's FIFO; the UART is only claimed once the
     * MIDI is first switched on. */
    chord_out_modes(s_chord_out.Value(), &g_cm.mode, &g_cv_mode);
    write_jacks(o);

    g_cm.channel = s_chord_ch.Value();
    if(g_cm.mode != CHORD_MIDI_OFF) midiout::Init();
    chord_midi_update(&g_cm, &S, &g_cm_ring);
    midiout::Drain(&g_cm_ring);

    if(g_lp_mode)
    {
        /* Give the port a moment after boot before the host starts (Mark:
         * the Launchpad came up more reliably this way). */
        if(g_lp_stage == 0 && now - g_lp_boot_ms > 2300u)
        {
            lp::Init();
            g_lp_stage = 2;
        }
        else if(g_lp_stage >= 2)
        {
            lp::Poll(now);
            pad_poll();
            lp_events();
            g_lp_stage = (uint8_t)(2 + lp::Stage());
        }
    }

    if(settings.IsActive())
    {
        b2.down       = false;
        g_btn_swallow = (uint8_t)((1u << kButtonB1) | (1u << kButtonB2) | (1u << kButtonB3));
        return;
    }

    /* B2: play / stop, on release. Stands down while B3 is held: B2+B3 held
     * two seconds is the Settings chord. */
    const bool pressed = btn_live(kButtonB2);
    const bool shift   = hw.buttons[kButtonB3].Pressed();
    if(pressed && !b2.down)
    {
        b2.down = true;
        b2.veto = shift;
    }
    if(b2.down && shift) b2.veto = true;
    if(b2.down && !pressed)
    {
        b2.down = false;
        if(!b2.veto) toggle_play();
    }
}

/* ---- control frame (~60 Hz) ------------------------------------------------------------- */

/* 2 x 15 KB, compared four times a second: SDRAM is plenty fast for that.
 * Not zeroed at boot; both are written before they are read. */
static uint8_t DSY_SDRAM_BSS g_snap[SEQ_STATE_BYTES];
static uint8_t DSY_SDRAM_BSS g_saved[SEQ_STATE_BYTES];
static bool     g_dirty       = false;
static uint32_t g_dirty_since = 0;
static uint32_t g_last_snap   = 0;
static bool     prev_settings = false;

static void mark_dirty(uint32_t now)
{
    g_dirty       = true;
    g_dirty_since = now;
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();
    for(uint8_t p = 0; p < kNumPots; p++) g_phys[p] = hw.pots[p].Value();

    if(g_lp_mode && g_lp_stage >= 2) xl_frame();
    apply_setup();
    sync_voice_knobs();
    sync_drum_knobs();
    if(g_lp_mode && g_lp_stage >= 2) lp_paint(now);

    /* Anything that would be saved changed? Compare the state as it would
     * be written, four times a second: whatever changed it -- pots, grid,
     * XL, gamepad, a queued slot taking over -- this sees it. */
    if(now - g_last_snap >= 250u)
    {
        g_last_snap = now;
        seq_state_write(&S, g_snap);
        if(memcmp(g_snap, g_saved, SEQ_STATE_BYTES) != 0)
        {
            memcpy(g_saved, g_snap, SEQ_STATE_BYTES);
            mark_dirty(now);
        }
    }

    const bool sact = settings.IsActive();
    if(prev_settings && !sact) mark_dirty(now);
    prev_settings = sact;

    /* A USB port change goes to the card for every firmware, as soon as
     * Settings closes, independently of the autosave. */
    static int shared_written = -2;
    if(shared_written == -2) shared_written = (int)s_usb.Value();
    if(!sact && (int)s_usb.Value() != shared_written && !picker::Busy()
       && usbshared::Save(sd, (int)s_usb.Value(), now))
        shared_written = (int)s_usb.Value();

    /* Autosave: 3 s after the last change, hands off, and only while
     * STOPPED (the save pauses the 1 ms poll; see the header). A held
     * gamepad fill or mute must not be saved either. */
    if(g_dirty && !S.running && now - g_dirty_since >= 3000u && !sact && !picker::Busy()
       && g_pad_held == 0 && !hw.buttons[kButtonB1].Pressed()
       && !hw.buttons[kButtonB2].Pressed() && !hw.buttons[kButtonB3].Pressed())
    {
        presets.Save(kHomeSlot);
        g_dirty = false;
    }
}

/* ---- LEDs ---------------------------------------------------------------------------------- */

/*
 * The SDK draws the rings from the knobs. Added on top:
 *   B1 pair   the voice the pots are on: orange bass, cyan lead
 *   B2 pair   green while playing, blue flash on each J1 clock edge, dim
 *             when stopped; magenta flash every 2 s while there is unsaved
 *             work and the transport runs (stop to keep it)
 *   B3 pair   dim Setup tint (the SDK paints it while the page is held)
 */
static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if(settings.IsActive()) return;

    L.SetButtonPair(kButtonB1, L.ScaleGlobal(pager.Page() == kPageLead    ? kColLead
                                             : pager.Page() == kPageDrums ? kColDrums
                                                                          : kColBass));

    LedPanel::Rgb b2c = S.running ? kGreen : kIdle;
    if(t_ms < g_clock_flash_until) b2c = kBlue;
    if(g_dirty && S.running && (t_ms % 2000u) < 120u) b2c = kMagenta;
    if(t_ms < g_import_flash_until && (t_ms / 150u) % 2u == 0u)
        b2c = g_import_ok ? kGreen : kRedFlash;
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2c));

    if(pager.Page() != kPageSetup)
        L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColSetup));
}

/* ---- audio ----------------------------------------------------------------------------------- */

static void AudioCallback(daisy::AudioHandle::InputBuffer  /*in*/,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    /* J9/J10 are claimed as CV outs; the SDK's shim overwrites them with
     * the SetVolts targets after this returns. J1/J2 edges are detected by
     * the shim too. Silence otherwise. */
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = 0.0f;
        out[1][i] = 0.0f;
    }
}

/* ---- boot -------------------------------------------------------------------------------------- */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);

    /* The engine first: a preset load below overwrites what it keeps. */
    seq_init(&S, 2026u);
    extras.seq = &S;
    chord_midi_init(&g_cm);
    chord_midi_ring_init(&g_cm_ring);

    for(uint8_t j = 0; j < kNumCvJacks; j++)
    {
        hw.cv_jacks[j].EnableCvOutput();
        hw.cv_jacks[j].SetVolts(0.0f);
    }

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    /* Pots 2 and 3 are the SDK's preset slot / action pair; 0 is brightness. */
    s_usb = settings.Page(kSettingsMain).Pot(5)
        .Selector(kUsbLabels).Default(1)
        .Ident("usb").Name("USB port")
        .Help("**Launchpad**: the Lab is the USB host for a Launchpad Mini "
              "MK3, a Launch Control XL and a Haute42 gamepad (XInput), "
              "through a powered hub. **Mac**: the front USB-C is HostLink, "
              "for the web programmer, presets and the card. Takes effect "
              "when the module next powers up; hold B1 while powering up for "
              "Mac mode once.");
    s_chord_out = settings.Page(kSettingsMain).Pot(1)
        .Selector(kChordOutLabels).Default(0)
        .Ident("chord_out").Name("Chord out")
        .Help("The chord playing now, for Belt on a second Lab. **MIDI**: its "
              "notes on the rear header (USART1 TX, pin 8); **+ clock** adds "
              "MIDI clock, start and stop. Cable: pin 8 to the other Lab's "
              "pin 7, pin 7 to its pin 8, and grounds -- never a straight "
              "ribbon, the header carries +-12 V. **CV**: J10 carries the "
              "chord's root, 1 V/oct from 0 V = C, and Perc goes silent; Belt "
              "builds the chord in its own key and scale, so set both modules "
              "to the same KEY and SCALE. It changes on the bar and holds when "
              "stopped. **CV + MIDI** is both. **CV ref** holds J10 at 0 V for "
              "Belt's Learn.");
    s_chord_ch = settings.Page(kSettingsMain).Pot(4)
        .Selector(kChannelLabels).Default(0)
        .Ident("chord_ch").Name("Chord channel")
        .Help("The MIDI channel chord out sends on.");

    host.Product("seq")
        .BootSlot(kHomeSlot)
        .Pages(bass_page, lead_page, drums_page, setup_page)
        .Jacks(jk_j1, jk_j2, jk_j3, jk_j4, jk_j5, jk_j6, jk_j7, jk_j8, jk_j9, jk_j10)
        .Buttons(bt_voice, bt_play, bt_setup)
        .Attach(manual)
        .Extend(fs_ext);

    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    const bool had_home = presets.HasValid(kHomeSlot);
    presets.BootLoad();
    if(had_home) presets.Load(kHomeSlot);

    /* Physical pot positions, primed. */
    for(int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for(uint8_t p = 0; p < kNumPots; p++) g_phys[p] = hw.pots[p].Value();

    if(!had_home)
    {
        /* First boot after a flash: the setup page at the engine's
         * defaults (120 BPM, straight, 60 ms glide, A minor, 16ths). Zone
         * i of N sits at (i + 0.5) / N. */
        pager.SetStored(kPageSetup, kPotTopLeft,     (120.f - 30.f) / 270.f, g_phys);
        pager.SetStored(kPageSetup, kPotTopRight,    0.0f, g_phys);
        pager.SetStored(kPageSetup, kPotMiddleLeft,  logf(60.f) / logf(1000.f), g_phys);
        pager.SetStored(kPageSetup, kPotMiddleRight, 9.5f / 12.f, g_phys);
        pager.SetStored(kPageSetup, kPotBottomLeft,  1.5f / (float)SEQ_SCALE_COUNT, g_phys);
        pager.SetStored(kPageSetup, kPotBottomRight, 2.5f / 4.f, g_phys);
    }

    /* The voice pages show the current slots' knobs, catch armed: after a
     * picker switch the pots point wherever the last firmware left them. */
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        const seq_knobs_t* k = seq_knobs(&S, v);
        g_mute_v[v] = S.v[v].mute;
        for(int p = 0; p < 6; p++)
        {
            g_applied[v][p] = knob_norm(k, p);
            pager.SetStored(kVoicePage[v], (uint8_t)p, g_applied[v][p], g_phys);
        }
    }
    for(int d = 0; d < SEQ_DRUMS; d++) g_mute_d[d] = S.drum_mute[d];
    seq_state_write(&S, g_saved);

    /* The front port's role is one setting for every firmware on the card:
     * adopt the card's value (Audio, which this firmware has not got, is
     * Mac here). B1 held through power-up: Mac mode this boot. */
    const int shared_usb = usbshared::Load(sd, 500);
    if(shared_usb >= 0)
    {
        const int want = shared_usb == 1 ? 1 : 0;
        if(want != (int)s_usb.Value()) s_usb.Default((uint8_t)want);
    }
    for(int i = 0; i < 20; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    const bool force_mac = hw.buttons[kButtonB1].Pressed();

    /* The drum file, after the saved snapshot above so an import counts as
     * a change and autosaves; then the DRUMS page shows what plays. */
    import_drums_file(System::GetNow());
    {
        const seq_drums_t* dr = seq_drums(&S);
        for(int p = 0; p < 4; p++) g_dapplied[p] = dr->gen[p];
        g_dapplied[4] = zone_norm(dr->style, SEQ_DSTYLE_COUNT);
        g_dapplied[5] = zone_norm(S.drum_queued, SEQ_DRUM_SLOTS);
        for(int p = 0; p < 6; p++)
            pager.SetStored(kPageDrums, (uint8_t)p, g_dapplied[p], g_phys);
    }
    g_lp_mode    = (int)s_usb.Value() == 1 && !force_mac;
    g_lp_boot_ms = System::GetNow();

    chord_cv_init(&g_cv, &S); /* hold chord 1's root until it plays */
    hw.StartAudio(AudioCallback);

    loop.Use(pager)
        .Use(settings)
        .Use(bass_page)
        .Use(lead_page)
        .Use(drums_page)
        .Use(setup_page)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);
    if(!g_lp_mode) loop.Use(host);

    for(;;) loop.Tick();
}
