/* Percolator STEMS, offline checks (development; 2.1.0). Built natively against the engine, without the wrapper:
 *   cc -O1 -g -fsanitize=address,undefined -Isrc mpc/stems_test.c src/percolator.c src/voices.c src/kits.c \
 *      -lm -lpthread -ldl -o build/stems_test && build/stems_test build/stems_out
 * With "--out <file>" it only renders the reference groove (STEMS off) to <file> as raw int16, so a build of another
 * version can be compared with this one (bit for bit). Prints PASSED or the failures. */
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    void *(*create)(const char *data_dir);
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);
    void (*render)(void *inst, int16_t *out_lr, int frames);
} mpc_engine_t;
const mpc_engine_t *mpc_engine(void);
void mpc_engine_transport(void *inst, double bpm, double ppq, int playing) __attribute__((weak));

static const mpc_engine_t *E;
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void midi3(void *I, int a, int b, int c) {
    uint8_t m[3] = {(uint8_t)a, (uint8_t)b, (uint8_t)c};
    E->midi(I, m, 3);
}
static void set(void *I, const char *k, const char *v) { E->set_param(I, k, v); }
static void status(void *I, char *b) { b[0] = 0; E->get_param(I, "stems_status", b, 64); }

/* the output, mono (left), of every block rendered by out_add */
static int16_t *obuf;
static int opos, ocap;
static double ppq;
static int playing, use_tp = 1;   /* use_tp 0: the host never tells the engine its transport (an older wrapper) */

static void block(void *I, void *tap) {
    if (mpc_engine_transport && use_tp) {
        mpc_engine_transport(I, 120.0, ppq, playing);
        if (tap) mpc_engine_transport(tap, 120.0, ppq, playing);
    }
    int16_t b[256];
    E->render(I, b, 128);
    if (tap) { int16_t t[256]; E->render(tap, t, 128); }
    if (opos + 128 > ocap) { ocap = ocap ? ocap * 2 : 1 << 20; obuf = realloc(obuf, ocap * sizeof *obuf); }
    for (int i = 0; i < 128; i++) obuf[opos + i] = b[2 * i];
    opos += 128;
    if (playing) ppq += 128.0 / 44100.0 * 2.0;   /* 120 BPM */
}
static void run(void *I, void *tap, int frames) { for (; frames > 0; frames -= 128) block(I, tap); }

/* two bars of all four voices, then a second of silence for the tails */
static void groove(void *I, void *tap) {
    static const char *const PAT[4] = {"x.......x.x.....", "......x.......x.", "....x.......x...", "x.x.x.x.x.x.x.xx"};
    for (int bar = 0; bar < 2; bar++)
        for (int st = 0; st < 16; st++) {
            for (int vo = 0; vo < 4; vo++) if (PAT[vo][st] == 'x') midi3(I, 0x90, 36 + vo, vo == 3 && (st & 2) ? 80 : 115);
            run(I, tap, 5504);
            for (int vo = 0; vo < 4; vo++) if (PAT[vo][st] == 'x') midi3(I, 0x80, 36 + vo, 0);
        }
}

/* ---- WAV files ------------------------------------------------------------------------------------------------- */
typedef struct { float *x; int n; } wav_t;

static int rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static wav_t wav_read(const char *path) {
    wav_t w = {0, -1};
    FILE *f = fopen(path, "rb");
    if (!f) return w;
    unsigned char h[58];
    if (fread(h, 1, 58, f) == 58 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && h[20] == 3 && h[22] == 1 &&
        rd32(h + 24) == 44100 && h[34] == 32 && !memcmp(h + 50, "data", 4)) {
        int bytes = rd32(h + 54);
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if (size == 58 + bytes && rd32(h + 4) == size - 8 && rd32(h + 46) == bytes / 4) {
            w.n = bytes / 4;
            w.x = malloc(bytes + 4);
            fseek(f, 58, SEEK_SET);
            if (fread(w.x, 4, w.n, f) != (size_t)w.n) w.n = -1;
        }
    }
    fclose(f);
    return w;
}

/* the take folders under base, sorted */
static int takes(const char *base, char names[][300], int max) {
    DIR *d = opendir(base);
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max)
        if (e->d_name[0] != '.') snprintf(names[n++], 300, "%s/%s", base, e->d_name);
    closedir(d);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) { char t[300]; memcpy(t, names[i], 300); memcpy(names[i], names[j], 300); memcpy(names[j], t, 300); }
    return n;
}

static const char *const CH[6] = {"V1", "V2", "V3", "V4", "FX", "MIX"};
static int load_take(const char *dir, wav_t *w) {
    int n = -2;
    for (int c = 0; c < 6; c++) {
        char p[400];
        snprintf(p, sizeof p, "%s/Percolator %s.wav", dir, CH[c]);
        w[c] = wav_read(p);
        if (w[c].n < 0) return -1;
        if (n == -2) n = w[c].n;
        else if (w[c].n != n) return -1;
    }
    return n;
}
static void free_take(wav_t *w) { for (int c = 0; c < 6; c++) free(w[c].x); }

static void rmtree(const char *p) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd)) {}
}

static void wait_saved(void *I) {   /* the worker closes a take within 20 ms; give it up to 3 s */
    char s[64];
    for (int i = 0; i < 300; i++) {
        status(I, s);
        if (!strncmp(s, "SAVED", 5) || !strncmp(s, "CAN'T", 5) || !strncmp(s, "CARD", 4)) return;
        usleep(10000);
    }
}

/* ---- the checks ------------------------------------------------------------------------------------------------- */

/* a take of the groove in a mode; returns frames of the take, fills w */
static int take_of(const char *base, const char *mode, int comp, int tapv1, wav_t *w, int16_t **out, int *out_at, int *nout) {
    rmtree(base);
    void *I = E->create(NULL), *tap = NULL;
    set(I, "kit", "1");
    run(I, NULL, 256);
    if (comp) { set(I, "comp_thresh", "70"); set(I, "comp_amt", "80"); set(I, "comp_level", "60"); }
    set(I, "v2_fx", "60");   /* echoes in the FX stem */
    set(I, "volume", "60");
    if (tapv1) {
        for (int v = 2; v <= 4; v++) { char k[16]; snprintf(k, sizeof k, "v%d_out", v); set(I, k, "0"); }
        tap = E->create(NULL);
        set(tap, "output", "1");
    } else
        for (int v = 1; v <= 4; v++) { char k[16]; snprintf(k, sizeof k, "v%d_out", v); set(I, k, "0"); }
    set(I, "stems", mode);
    playing = 0; ppq = 0;
    opos = 0;
    run(I, tap, 4096);
    *out_at = opos;
    playing = 1;
    groove(I, tap);
    playing = 0;
    run(I, tap, 44100 * 4);
    wait_saved(I);
    *nout = opos - *out_at;
    *out = obuf + *out_at;
    if (tap) E->destroy(tap);
    E->destroy(I);
    char names[8][300];
    int nt = takes(base, names, 8);
    CHECK(nt == 1, "%s: %d takes", mode, nt);
    return nt == 1 ? load_take(names[0], w) : -1;
}

static void test_sums(const char *base) {
    for (int comp = 0; comp <= 1; comp++)
        for (int m = 1; m <= 2; m++) {
            wav_t w[6];
            int16_t *o;
            int at, no;
            int n = take_of(base, m == 1 ? "1" : "2", comp, 0, w, &o, &at, &no);
            CHECK(n > 44100 * 4, "mode %d comp %d: take of %d frames", m, comp, n);
            if (n <= 0) continue;
            /* the take starts with PLAY: its first frame is the first one rendered while playing */
            double err_sum = 0, err_out = 0, pk[6] = {0};
            int cnt = 0;
            for (int i = 0; i < n && i < no; i++) {
                float s = w[0].x[i] + w[1].x[i] + w[2].x[i] + w[3].x[i] + w[4].x[i], mix = w[5].x[i];
                for (int c = 0; c < 6; c++) if (fabsf(w[c].x[i]) > pk[c]) pk[c] = fabsf(w[c].x[i]);
                if (fabsf(mix) < 0.85f) { double e = fabs(s - mix); if (e > err_sum) err_sum = e; cnt++; }
                double eo = fabs(mix * 32767.0 - o[i]);
                if (eo > err_out) err_out = eo;
            }
            for (int c = 0; c < 6; c++) CHECK(pk[c] > 0.001, "mode %d comp %d: %s silent (peak %g)", m, comp, CH[c], pk[c]);
            CHECK(err_out <= 0.51, "mode %d comp %d: MIX vs the output: %.2f LSB", m, comp, err_out);
            if (m == 2 || !comp) CHECK(err_sum < 2e-5, "mode %d comp %d: V1-V4 + FX vs MIX: %g", m, comp, err_sum);
            else CHECK(err_sum > 1e-3, "DRY with the compressor: the stems shouldn't add up to MIX (%g)", err_sum);
            /* the tail: the take ends after STOP once it's quiet, and before the 4 s rendered after STOP run out */
            int groove_frames = 2 * 16 * 5504;
            CHECK(n > groove_frames && n < groove_frames + 44100 * 4, "mode %d comp %d: take %d frames, groove %d", m, comp, n, groove_frames);
            float last = 0;
            for (int i = n - 22050; i < n; i++) for (int c = 0; c < 6; c++) if (fabsf(w[c].x[i]) > last) last = fabsf(w[c].x[i]);
            CHECK(last < 1e-5f, "mode %d comp %d: the take's last half second isn't silent (%g)", m, comp, last);
            printf("  mode %s comp %d: %d frames (%.2f s), peaks V1 %.2f V2 %.2f V3 %.2f V4 %.2f FX %.2f MIX %.2f, sum err %.1e, MIX vs out %.2f LSB\n",
                   m == 1 ? "DRY     " : "AS HEARD", comp, n, n / 44100.0, pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], err_sum, err_out);
            free_take(w);
        }
}

/* voice 1 on OUT 1, played by a second instance: V1 is still recorded, MIX (this track) leaves it out; the main
 * output is held a block back while an OUT plays, MIX isn't (it lines up with the other files) */
static void test_tap(const char *base) {
    wav_t w[6];
    int16_t *o;
    int at, no;
    int n = take_of(base, "2", 1, 1, w, &o, &at, &no);
    CHECK(n > 0, "tap: no take");
    if (n <= 0) return;
    double err_sum = 0, err_out = 0, pk1 = 0;
    for (int i = 0; i < n; i++) {
        float s = w[1].x[i] + w[2].x[i] + w[3].x[i] + w[4].x[i];
        if (fabsf(w[5].x[i]) < 0.85f && fabs(s - w[5].x[i]) > err_sum) err_sum = fabs(s - w[5].x[i]);
        if (fabsf(w[0].x[i]) > pk1) pk1 = fabsf(w[0].x[i]);
        if (i + 128 < no && fabs(w[5].x[i] * 32767.0 - o[i + 128]) > err_out) err_out = fabs(w[5].x[i] * 32767.0 - o[i + 128]);
    }
    CHECK(pk1 > 0.01, "tap: V1 silent");
    CHECK(err_sum < 2e-5, "tap: V2-V4 + FX vs MIX %g", err_sum);
    CHECK(err_out <= 0.51, "tap: MIX vs the output a block later: %.2f LSB", err_out);
    printf("  voice 1 on OUT 1: V1 peak %.2f, V2-V4 + FX = MIX to %.1e, MIX = output a block later to %.2f LSB\n", pk1, err_sum, err_out);
    free_take(w);
}

/* transport: nothing before PLAY, a take per PLAY, PLAY during the tail starts a new one; STEMS off ends a take at
 * once; a tap instance never records; the status text along the way */
static void test_transport(const char *base) {
    rmtree(base);
    char s[64], names[8][300];
    void *I = E->create(NULL);
    set(I, "stems", "1");
    status(I, s);
    CHECK(!strcmp(s, "ARMED: PLAY RECORDS"), "armed status \"%s\"", s);
    playing = 0;
    opos = 0;
    for (int k = 0; k < 20; k++) { midi3(I, 0x90, 36, 120); run(I, NULL, 4096); }   /* pads while stopped */
    usleep(60000);
    CHECK(takes(base, names, 8) == 0, "a take without PLAY");
    playing = 1;
    midi3(I, 0x90, 36, 120);
    run(I, NULL, 44100);
    status(I, s);
    CHECK(!strcmp(s, "REC 0:01"), "recording status \"%s\"", s);
    playing = 0;
    run(I, NULL, 1280);
    status(I, s);
    CHECK(strstr(s, "ENDING") != NULL, "tail status \"%s\"", s);
    playing = 1;   /* PLAY again during the tail: the first take ends, a second begins */
    run(I, NULL, 44100);
    playing = 0;
    run(I, NULL, 44100 * 2);
    wait_saved(I);
    status(I, s);
    CHECK(!strncmp(s, "SAVED ", 6) && strstr(s, "ARMED"), "saved status \"%s\"", s);
    usleep(1100000);   /* a take's folder is named by the second it started */
    int nt = takes(base, names, 8);
    CHECK(nt == 2, "PLAY, STOP, PLAY, STOP: %d takes", nt);
    if (nt == 2) {
        wav_t w[6];
        int n1 = load_take(names[0], w);
        CHECK(n1 == 44100 / 128 * 128 + 128 + 1280, "first take: %d frames (PLAY to the next PLAY)", n1);
        if (n1 > 0) free_take(w);
        int n2 = load_take(names[1], w);
        CHECK(n2 > 44100 && n2 < 44100 + 44100 / 2 + 44100, "second take: %d frames", n2);
        if (n2 > 0) free_take(w);
        char p[400];
        snprintf(p, sizeof p, "%s/take.txt", names[0]);
        FILE *f = fopen(p, "r");
        char txt[4096] = "";
        if (f) { size_t r = fread(txt, 1, sizeof txt - 1, f); txt[r] = 0; fclose(f); }
        CHECK(strstr(txt, "Tempo:      120.00 BPM") && strstr(txt, "Kit:        Init") && strstr(txt, "Stems:      DRY") &&
              strstr(txt, "Percolator V1.wav"), "take.txt:\n%s", txt);
    }
    /* STEMS off while playing ends the take at once (no tail) */
    rmtree(base);
    playing = 1;
    midi3(I, 0x90, 36, 120);
    run(I, NULL, 12800);
    set(I, "stems", "0");
    run(I, NULL, 12800);
    status(I, s);
    CHECK(!strcmp(s, "OFF"), "off status \"%s\"", s);
    set(I, "stems", "2");   /* on again while playing: a new take from here */
    run(I, NULL, 12800);
    set(I, "stems", "0");
    run(I, NULL, 128);
    usleep(1100000);
    playing = 0;
    nt = takes(base, names, 8);
    CHECK(nt == 2, "on, off, on, off while playing: %d takes", nt);
    for (int t = 0; t < nt && t < 2; t++) {
        wav_t w[6];
        int n = load_take(names[t], w);
        CHECK(n == 12800, "take %d: %d frames (switched off: no tail)", t, n);
        if (n > 0) free_take(w);
    }
    /* a tap instance: STEMS does nothing there */
    rmtree(base);
    set(I, "output", "1");
    set(I, "stems", "1");
    status(I, s);
    CHECK(!strcmp(s, "ONLY ON THE MAIN KIT"), "tap status \"%s\"", s);
    playing = 1;
    run(I, NULL, 12800);
    playing = 0;
    run(I, NULL, 44100);
    usleep(60000);
    CHECK(takes(base, names, 8) == 0, "a tap recorded");
    /* saved in the project, as any parameter */
    set(I, "output", "0");
    char st[16384];
    E->get_param(I, "state", st, sizeof st);
    CHECK(strstr(st, "stems=1;") != NULL, "STEMS not in the state");
    E->destroy(I);
    I = E->create(NULL);
    E->set_param(I, "state", st);
    status(I, s);
    CHECK(!strcmp(s, "ARMED: PLAY RECORDS"), "restored status \"%s\"", s);
    E->destroy(I);
    rmtree(base);
}

/* destroyed while recording (a track deleted, a project closed mid-song): the files are closed complete */
static void test_destroy(const char *base) {
    rmtree(base);
    void *I = E->create(NULL);
    set(I, "stems", "2");
    playing = 1;
    midi3(I, 0x90, 37, 120);
    run(I, NULL, 44800);
    E->destroy(I);
    playing = 0;
    char names[8][300];
    int nt = takes(base, names, 8);
    CHECK(nt == 1, "destroyed mid-take: %d takes", nt);
    wav_t w[6];
    int n = nt == 1 ? load_take(names[0], w) : -1;
    CHECK(n == 44800, "destroyed mid-take: %d frames", n);
    if (n > 0) free_take(w);
    rmtree(base);
}

/* no transport from the host (an older wrapper): the switch alone */
static void test_no_transport(const char *base) {
    rmtree(base);
    use_tp = 0;
    void *I = E->create(NULL);
    run(I, NULL, 1280);
    set(I, "stems", "1");   /* on: a take starts at once */
    midi3(I, 0x90, 38, 120);
    run(I, NULL, 25600);
    char s[64];
    status(I, s);
    CHECK(!strncmp(s, "REC 0:00", 8), "no transport: status \"%s\"", s);
    set(I, "stems", "0");   /* off: it ends */
    run(I, NULL, 1280);
    usleep(80000);
    E->destroy(I);
    use_tp = 1;
    char names[4][300];
    int nt = takes(base, names, 4);
    CHECK(nt == 1, "no transport: %d takes", nt);
    wav_t w[6];
    int n = nt == 1 ? load_take(names[0], w) : -1;
    CHECK(n == 25600 - 5 * 128, "no transport: %d frames (from the 16th block on)", n);
    if (n > 0) free_take(w);
    if (nt == 1) {
        char p[400], txt[4096] = "";
        snprintf(p, sizeof p, "%s/take.txt", names[0]);
        FILE *f = fopen(p, "r");
        if (f) { size_t r = fread(txt, 1, sizeof txt - 1, f); txt[r] = 0; fclose(f); }
        CHECK(strstr(txt, "the STEMS switch") && !strstr(txt, "Tempo:"), "no transport take.txt:\n%s", txt);
    }
    rmtree(base);
}

/* STEMS on in a project that's loading: the first blocks come before the host's first transport call, and must
 * not start a take of their own */
static void test_first_blocks(const char *base) {
    rmtree(base);
    void *I = E->create(NULL);
    set(I, "stems", "1");
    use_tp = 0;
    run(I, NULL, 3 * 128);   /* rendered before the transport is heard of */
    use_tp = 1;
    playing = 0;
    run(I, NULL, 44100);
    usleep(60000);
    char names[4][300];
    CHECK(takes(base, names, 4) == 0, "a take before the transport was heard of");
    E->destroy(I);
    rmtree(base);
}

/* a card that stalls: the frames that don't fit come back as silence, so the timeline holds */
#include "../src/stemrec.h"
static void test_gap(const char *base) {
    rmtree(base);
    static stemrec_t S;
    memset(&S, 0, sizeof S);
    const char *names[2] = {"A", "B"};
    CHECK(!sr_init(&S, "T", 2, names, NULL, base), "sr_init");
    sr_arm(&S, 1);
    sr_meta_t m;
    memset(&m, 0, sizeof m);
    m.ppq = -1;
    snprintf(m.label, sizeof m.label, "gap:test/1");   /* characters a card can't take in a folder name */
    sr_start(&S, &m);
    usleep(50000);   /* the worker opens the take */
    pthread_mutex_lock(&S.mu);   /* ... and then can't drain: the card "stalls" */
    usleep(50000);
    float buf[2 * 128];
    uint32_t fr = 0, total = SR_RING + 40 * 128 + 64 * 128;
    while (fr < total) {
        for (int i = 0; i < 128; i++) { buf[2 * i] = (float)(fr + i); buf[2 * i + 1] = -(float)(fr + i); }
        sr_write(&S, buf, 128);
        fr += 128;
        if (fr == SR_RING + 40 * 128) { pthread_mutex_unlock(&S.mu); usleep(300000); }   /* it catches up */
    }
    sr_stop(&S);
    sr_free(&S);
    char names2[4][300];
    int nt = takes(base, names2, 4);
    CHECK(nt == 1 && strstr(names2[0], "gap-test-1"), "gap: %d takes (%s)", nt, nt ? names2[0] : "");
    if (nt != 1) return;
    char p[400];
    snprintf(p, sizeof p, "%s/T A.wav", names2[0]);
    wav_t a = wav_read(p);
    snprintf(p, sizeof p, "%s/T B.wav", names2[0]);
    wav_t b = wav_read(p);
    CHECK(a.n == (int)total && b.n == (int)total, "gap: lengths %d %d, wrote %u", a.n, b.n, total);
    int ok = 1, zeros = 0, first_gap = -1;
    for (int i = 0; i < a.n && i < b.n; i++) {
        if (a.x[i] == 0 && b.x[i] == 0 && i > 0) { zeros++; if (first_gap < 0) first_gap = i; continue; }
        if (a.x[i] != (float)i || b.x[i] != -(float)i) { ok = 0; printf("  frame %d: %g %g\n", i, a.x[i], b.x[i]); break; }
    }
    CHECK(ok, "gap: every frame that was written is where it belongs");
    CHECK(zeros > 0 && first_gap >= (int)SR_RING - 128, "gap: %d frames of silence from %d", zeros, first_gap);
    snprintf(p, sizeof p, "%s/take.txt", names2[0]);
    FILE *f = fopen(p, "r");
    char txt[4096] = "";
    if (f) { size_t r = fread(txt, 1, sizeof txt - 1, f); txt[r] = 0; fclose(f); }
    CHECK(strstr(txt, "Gaps:") != NULL, "gap: take.txt doesn't count it");
    printf("  stalled card: %d frames written as silence from frame %d, the rest in place\n", zeros, first_gap);
    free(a.x);
    free(b.x);
    rmtree(base);
}

/* the card can't be written: the status says so, nothing crashes, the sound goes on */
static void test_unwritable(void) {
    setenv("PERCOLATOR_STEMS_DIR", "/proc/no-such-place/stems", 1);
    void *I = E->create(NULL);
    set(I, "stems", "1");
    playing = 1;
    midi3(I, 0x90, 36, 120);
    run(I, NULL, 12800);
    usleep(80000);
    char s[64];
    status(I, s);
    CHECK(!strcmp(s, "CAN'T WRITE TO THE CARD"), "unwritable status \"%s\"", s);
    playing = 0;
    run(I, NULL, 44100 * 2);
    wait_saved(I);
    status(I, s);
    CHECK(!strcmp(s, "CAN'T WRITE TO THE CARD"), "unwritable status after STOP \"%s\"", s);
    E->destroy(I);
    playing = 0;
}

/* STEMS on doesn't change the sound by a bit */
static void test_same_sound(const char *base) {
    int16_t *ref = NULL, *o;
    int nref = 0;
    for (int on = 0; on <= 2; on++) {
        rmtree(base);
        void *I = E->create(NULL);
        set(I, "kit", "3");
        set(I, "comp_thresh", "60"); set(I, "comp_amt", "70");
        set(I, "stems", on == 0 ? "0" : on == 1 ? "1" : "2");
        playing = 1; ppq = 0; opos = 0;
        run(I, NULL, 256);
        groove(I, NULL);
        playing = 0;
        run(I, NULL, 44100);
        E->destroy(I);
        o = obuf;
        if (!on) { nref = opos; ref = malloc(nref * sizeof *ref); memcpy(ref, o, nref * sizeof *ref); }
        else CHECK(opos == nref && !memcmp(ref, o, nref * sizeof *ref), "STEMS %d changed the output", on);
    }
    free(ref);
    rmtree(base);
}

static void render_ref(const char *path) {   /* STEMS off: for comparing with 2.0.0 */
    FILE *f = fopen(path, "wb");
    for (int k = 0; k < 8; k++)
        for (int comp = 0; comp <= 1; comp++) {
            void *I = E->create(NULL);
            char v[8];
            snprintf(v, sizeof v, "%d", k);
            set(I, "kit", v);
            run(I, NULL, 256);
            if (comp) { set(I, "comp_thresh", "50"); set(I, "comp_amt", "60"); }
            playing = 1; ppq = 0; opos = 0;
            groove(I, NULL);
            playing = 0;
            run(I, NULL, 44100);
            fwrite(obuf, sizeof *obuf, opos, f);
            E->destroy(I);
        }
    fclose(f);
}

int main(int argc, char **argv) {
    E = mpc_engine();
    if (argc == 3 && !strcmp(argv[1], "--out")) { render_ref(argv[2]); return 0; }
    const char *base = argc > 1 ? argv[1] : "build/stems_out";
    char abs_base[512];
    snprintf(abs_base, sizeof abs_base, "%s", base);
    setenv("PERCOLATOR_STEMS_DIR", abs_base, 1);
    printf("stems: sums and levels\n");
    test_sums(base);
    printf("stems: a voice on an OUT\n");
    test_tap(base);
    printf("stems: transport, switch, status, state\n");
    test_transport(base);
    printf("stems: destroyed mid-take\n");
    test_destroy(base);
    printf("stems: output unchanged\n");
    test_same_sound(base);
    printf("stems: no transport from the host\n");
    test_no_transport(base);
    printf("stems: the first blocks of a loading project\n");
    test_first_blocks(base);
    printf("stems: a stalled card\n");
    test_gap(base);
    printf("stems: an unwritable card\n");
    test_unwritable();
    free(obuf);
    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
