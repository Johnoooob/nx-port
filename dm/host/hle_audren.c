// 音频渲染器（nn::audio::AudioRenderer）的宿主实现：另一个游戏用，条目都是可选的（理由以 ? 开头，只在试启动模式生效）。
// 真机上这些接口大多只改客户端工作区，再由 RequestUpdateAudioRenderer 经系统服务把状态换回来，混音在系统进程里做；宿主没有服务进程。
// 这里整组接管、不碰客户端工作区，自己做混音：
//   人声（Voice）：16 位 PCM（格式 2，多声道交织）或 ADPCM（格式 6，单声道，每 8 字节 14 个采样，系数来自 AcquireVoiceSlot 的参数、初始历史来自波形缓冲的上下文），
//                 按「采样率 × 音高 / 48000」线性插值重采样；挂上的波形缓冲播完即归还，带循环标志的原地循环。
//   混音（SubMix / FinalMix）：人声各声道按 SetVoiceMixVolume 的矩阵进目标混音的各路，子混音按自己的音量与矩阵进它的目标，最终混音的前两路是左右声道。
//   效果器（延迟、混响、辅助发送）只记账，不出效果。
// 出声：CoreAudio AudioUnit（Mac DefaultOutput / iOS RemoteIO），48 kHz 16 位立体声；回调里现混。开关与 hle_audio.c 一致：DM_AUDIO=1 真出声，iOS 缺省开、Mac 缺省关。
// 不出声时（Mac 缺省、无界面测试）：人声按真实时间「播完」并归还缓冲，游戏的声音流程（等一段声音播完、流式续缓冲）照常往下走。
// Host implementation of the audio renderer (nn::audio::AudioRenderer): for another game; every entry is optional (the reason starts with ?, active only in bring-up mode).
// On a real device most of these interfaces only modify the client work area, RequestUpdateAudioRenderer then exchanges state with the system service, and mixing happens in a system process; the host has no service processes.
// The whole group is taken over here without touching the client work area, and the mixing is done here:
//   Voices: 16-bit PCM (format 2, interleaved channels) or ADPCM (format 6, mono, 14 samples per 8 bytes, coefficients from the AcquireVoiceSlot parameter, initial history from the wave buffer's context),
//           resampled with linear interpolation by "sample rate x pitch / 48000"; an appended wave buffer is returned once played, one with the loop flag loops in place.
//   Mixes (SubMix / FinalMix): each voice channel goes into the buses of its destination mix through the SetVoiceMixVolume matrix, a submix goes into its destination with its own volume and matrix, and the first two buses of the final mix are left and right.
//   Effects (delay, reverb, aux send) are only bookkept; they produce no effect.
// Output: CoreAudio AudioUnit (Mac DefaultOutput / iOS RemoteIO), 48 kHz 16-bit stereo; mixed inside the callback. Same switch as hle_audio.c: DM_AUDIO=1 for real output, on by default on iOS and off by default on Mac.
// Without output (Mac default, headless tests): voices "finish" in real time and return their buffers, so the game's sound flow (waiting for a sound to finish, refilling streaming buffers) proceeds as usual.
#include "hle.h"
#include <unistd.h>
#include <TargetConditionals.h>
#include <AudioToolbox/AudioToolbox.h>
void hle_bind_system_event(void *ev, int autoclear);
void hle_signal_system_event(void *ev);

enum { AR_RATE = 48000, AR_NB = 8, AR_MAXF = 4096, AR_Q = 4, AR_VCH = 6 };
typedef struct { const void *buf; uint64_t size; int32_t start, end; uint8_t loop, eos, pad[6]; const void *ctx; uint64_t ctx_size; } ar_wave;
typedef struct { const ar_wave *w; const int16_t *pcm; int16_t *dec; int n; } ar_ent;   // pcm：可直接读的 16 位采样（PCM 指向游戏内存，ADPCM 指向 dec）；n：每声道采样数 | pcm: directly readable 16-bit samples (PCM points into game memory, ADPCM points to dec); n: samples per channel
typedef struct ar_mix { struct ar_mix *next; const void *key, *dest; int nbuf, is_final; float vol, mv[AR_NB][AR_NB]; float *acc; } ar_mix;
typedef struct ar_voice { struct ar_voice *next; const void *key, *dest; int rate, ch, fmt, state; float pitch, mv[AR_VCH][AR_NB]; int16_t coef[16];
                          ar_ent q[AR_Q]; int nq; const ar_wave *rel[8]; int nrel; double pos; int64_t played; } ar_voice;
static pthread_mutex_t g_ar = PTHREAD_MUTEX_INITIALIZER;
// DM_AUDREN_LOG=1：记前 400 条配置调用（人声 / 混音的建立、去向、音量矩阵），核对混音拓扑用 | DM_AUDREN_LOG=1: log the first 400 configuration calls (creation, destination and volume matrices of voices / mixes), for checking the mix topology
static int ar_log(void) { static int on = -1, n; if (on < 0) on = getenv("DM_AUDREN_LOG") != NULL; return on && n++ < 400; }
#define ALOG(...) do { if (ar_log()) LOG("audren " __VA_ARGS__); } while (0)
static ar_voice *g_ar_voices; static ar_mix *g_ar_mixes; static uint64_t g_ar_last; static void *g_ar_ev; static AudioUnit g_ar_au; static int g_ar_real;
static struct { const void *pool; int st; } g_ar_pool[256];

static ar_voice *ar_find(const void *key) { for (ar_voice *v = g_ar_voices; v; v = v->next) if (v->key == key) return v; return NULL; }
static ar_mix *ar_mix_find(const void *key) { for (ar_mix *m = g_ar_mixes; m; m = m->next) if (m->key == key) return m; return NULL; }
static void ar_ent_drop(ar_ent *e) { free(e->dec); e->dec = NULL; }
static void ar_pop(ar_voice *v) {   // 队首播完：归还并前移 | Head of the queue finished: return it and shift
    if (v->nrel < 8) v->rel[v->nrel++] = v->q[0].w;
    ar_ent_drop(&v->q[0]); memmove(v->q, v->q + 1, sizeof v->q[0] * (size_t)--v->nq); v->pos = 0;
}
static void ar_flush(ar_voice *v) { while (v->nq) ar_pop(v); }

// ADPCM（任天堂 DSP 格式）整块解码：每帧 1 字节头（高 4 位预测器号、低 4 位移位量）+ 14 个 4 位采样
// ADPCM (Nintendo DSP format) whole-buffer decode: each frame is a 1-byte header (high 4 bits predictor index, low 4 bits shift) + 14 4-bit samples
static int16_t *ar_adpcm(const ar_voice *v, const ar_wave *w, int *n) {
    const uint8_t *d = w->buf; uint64_t frames = w->size / 8; int total = (int)(frames * 14);
    int16_t *o = malloc(sizeof *o * (size_t)(total ? total : 1)); if (!o) return NULL;
    int h1 = 0, h2 = 0;
    if (w->ctx && w->ctx_size >= 6) { int16_t c[3]; memcpy(c, w->ctx, 6); h1 = c[1]; h2 = c[2]; }
    for (uint64_t f = 0; f < frames; f++) {
        uint8_t hd = d[f * 8]; int pr = (hd >> 4) & 7, sc = 1 << (hd & 15), c1 = v->coef[pr * 2], c2 = v->coef[pr * 2 + 1];
        for (int k = 0; k < 14; k++) {
            uint8_t b = d[f * 8 + 1 + k / 2]; int nib = (k & 1) ? b & 15 : b >> 4; if (nib >= 8) nib -= 16;
            int s = ((nib * sc) << 11) + 1024 + c1 * h1 + c2 * h2; s >>= 11;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            o[f * 14 + k] = (int16_t)s; h2 = h1; h1 = s;
        }
    }
    *n = total; return o;
}
static int ar_ent_make(const ar_voice *v, const ar_wave *w, ar_ent *e) {
    e->w = w; e->dec = NULL;
    if (v->fmt == 6) { e->dec = ar_adpcm(v, w, &e->n); if (!e->dec) return 0; e->pcm = e->dec; }
    else { e->pcm = w->buf; e->n = (int)(w->size / (2 * (uint64_t)(v->ch > 0 ? v->ch : 1))); }
    if (w->end > w->start && w->end <= e->n) e->n = w->end;   // 有效区间 [start, end) | Valid range [start, end)
    return 1;
}
static int ar_start(const ar_ent *e) { return e->w->start > 0 && e->w->start < e->n ? e->w->start : 0; }

// 一个人声出 n 帧到 tmp[声道][帧]（48 kHz）；返回是否出了声 | Render n frames of one voice into tmp[channel][frame] (48 kHz); returns whether it produced anything
static int ar_voice_render(ar_voice *v, float (*tmp)[AR_MAXF], int n) {
    int ch = v->fmt == 6 ? 1 : (v->ch > AR_VCH ? AR_VCH : v->ch), stride = v->fmt == 6 ? 1 : v->ch; if (ch < 1) return 0;
    double step = (double)v->rate * (v->pitch > 0 ? v->pitch : 1) / AR_RATE; int any = 0;
    for (int c = 0; c < ch; c++) memset(tmp[c], 0, sizeof(float) * (size_t)n);
    for (int i = 0; i < n && v->nq; i++) {
        ar_ent *e = &v->q[0]; int s0 = ar_start(e), len = e->n - s0;
        if (len <= 0) { ar_pop(v); i--; continue; }
        if (v->pos >= len) { if (e->w->loop) v->pos -= len; else { v->pos -= len; double keep = v->pos; ar_pop(v); v->pos = keep; } i--; continue; }
        int i0 = (int)v->pos, i1 = i0 + 1 < len ? i0 + 1 : i0; float fr = (float)(v->pos - i0);
        for (int c = 0; c < ch; c++) { float a = e->pcm[(size_t)(s0 + i0) * stride + c], b = e->pcm[(size_t)(s0 + i1) * stride + c]; tmp[c][i] = (a + (b - a) * fr) * (1.0f / 32768.0f); }
        v->pos += step; v->played++; any = 1;
    }
    return any;
}
static int ar_depth(const ar_mix *m) { int d = 0; while (m && !m->is_final && d < 8) { m = ar_mix_find(m->dest); d++; } return d; }
static void ar_mix_frames(int16_t *out, int n) {   // 持锁调用；out：立体声交织 | Called with the lock held; out: interleaved stereo
    static float tmp[AR_VCH][AR_MAXF];
    for (ar_mix *m = g_ar_mixes; m; m = m->next) for (int b = 0; b < AR_NB; b++) memset(m->acc + (size_t)b * AR_MAXF, 0, sizeof(float) * (size_t)n);   // 各路在缓冲里相隔 AR_MAXF：逐路清 | Buses are AR_MAXF apart in the buffer: clear bus by bus
    for (ar_voice *v = g_ar_voices; v; v = v->next) {
        if (v->state != 0 || !v->nq) continue;   // 0 = 播放 | 0 = playing
        ar_mix *d = ar_mix_find(v->dest); int ch = v->fmt == 6 ? 1 : (v->ch > AR_VCH ? AR_VCH : v->ch);
        if (!ar_voice_render(v, tmp, n) || !d) continue;
        for (int s = 0; s < ch; s++) for (int b = 0; b < AR_NB; b++) { float g = v->mv[s][b]; if (g == 0) continue; float *a = d->acc + (size_t)b * AR_MAXF; for (int i = 0; i < n; i++) a[i] += tmp[s][i] * g; }
    }
    for (int depth = 8; depth >= 1; depth--)
        for (ar_mix *m = g_ar_mixes; m; m = m->next) {
            if (m->is_final || ar_depth(m) != depth) continue;
            ar_mix *d = ar_mix_find(m->dest); if (!d) continue;
            for (int s = 0; s < AR_NB; s++) for (int b = 0; b < AR_NB; b++) { float g = m->mv[s][b] * m->vol; if (g == 0) continue;
                const float *src = m->acc + (size_t)s * AR_MAXF; float *a = d->acc + (size_t)b * AR_MAXF; for (int i = 0; i < n; i++) a[i] += src[i] * g; }
        }
    memset(out, 0, sizeof(int16_t) * 2 * (size_t)n);
    for (ar_mix *m = g_ar_mixes; m; m = m->next) {
        if (!m->is_final) continue;
        const float *l = m->acc, *r = m->nbuf >= 2 ? m->acc + AR_MAXF : m->acc;
        for (int i = 0; i < n; i++) { float a = l[i] * m->vol * 32767.0f + out[2 * i], b = r[i] * m->vol * 32767.0f + out[2 * i + 1];
            out[2 * i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a); out[2 * i + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b); }
    }
}
static OSStatus ar_au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)ctx; (void)f; (void)ts; (void)bus; int16_t *out = bl->mBuffers[0].mData;
    pthread_mutex_lock(&g_ar);
    for (UInt32 done = 0; done < frames;) { int n = (int)(frames - done > AR_MAXF ? AR_MAXF : frames - done); ar_mix_frames(out + 2 * done, n); done += (UInt32)n; }
    pthread_mutex_unlock(&g_ar);
    // 诊断开关 DM_AUDREN_DUMP=<文件>：混好的输出原样续写（16 位、48 kHz、立体声交织），不靠耳朵也能核对有没有出声
    // Diagnostic switch DM_AUDREN_DUMP=<file>: the mixed output is appended as is (16-bit, 48 kHz, interleaved stereo), so "is there sound" can be checked without ears
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDREN_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】DM_AUDREN_DUMP 打不开 %s", e); }
      if (df) fwrite(out, 4, frames, df); }
    return noErr;
}
static int ar_real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : TARGET_OS_IPHONE; }
static void ar_au_open(void) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
                                   .componentSubType = kAudioUnitSubType_RemoteIO,
#else
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    AudioStreamBasicDescription fm = {.mSampleRate = AR_RATE, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                      .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {ar_au_render, NULL};
    if (!c || AudioComponentInstanceNew(c, &g_ar_au) || AudioUnitSetProperty(g_ar_au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fm, sizeof fm) ||
        AudioUnitSetProperty(g_ar_au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) || AudioUnitInitialize(g_ar_au) || AudioOutputUnitStart(g_ar_au)) {
        LOG("【红】audio：AudioUnit 起不来，退回不出声的时间模型"); g_ar_au = NULL; return; }
    g_ar_real = 1; LOG("audio：AudioUnit 已启动（48 kHz 立体声，宿主混音）");
}

static void *ar_tick_thread(void *a) { (void)a; for (;;) { usleep(5000); hle_signal_system_event(g_ar_ev); } return NULL; }
static void ar_open(void *ev) {
    LOG("audio：OpenAudioRenderer（宿主混音；%s）", ar_real_audio() ? "真出声" : "不出声，按时间消耗");
    if (!ev || g_ar_ev) return;
    hle_bind_system_event(ev, 1); g_ar_ev = ev;
    pthread_t th; if (pthread_create(&th, NULL, ar_tick_thread, NULL)) die("音频渲染器节拍线程起不来"); pthread_detach(th);
}
// 不出声但要核对混音（DM_AUDREN_DUMP 设了、DM_AUDIO 没开）：起一个线程按实时节奏每 10 ms 混 480 帧写进文件，扬声器不出任何声音
// No output but the mix is to be checked (DM_AUDREN_DUMP set, DM_AUDIO off): a thread mixes 480 frames every 10 ms in real time into the file; nothing reaches the speakers
static void *ar_dump_thread(void *a) {
    FILE *df = a; static int16_t out[2 * 480]; uint64_t next = now_ns();
    for (;;) { pthread_mutex_lock(&g_ar); ar_mix_frames(out, 480); pthread_mutex_unlock(&g_ar); fwrite(out, 4, 480, df);
               next += 10000000ull; uint64_t now = now_ns(); if (next > now) usleep((useconds_t)((next - now) / 1000)); }
    return NULL;
}
static void ar_startup(void) {
    if (ar_real_audio()) { if (!g_ar_au) ar_au_open(); return; }
    const char *e = getenv("DM_AUDREN_DUMP"); static int started;
    if (e && !started) { FILE *df = fopen(e, "wb"); pthread_t th; if (df && !pthread_create(&th, NULL, ar_dump_thread, df)) { pthread_detach(th); started = 1; g_ar_real = 1; LOG("audio：不出声，混音结果写入 %s", e); } }
}
static void ar_pool_set(const void *pool, int st) {
    pthread_mutex_lock(&g_ar); int fr = -1;
    for (int i = 0; i < 256; i++) { if (g_ar_pool[i].pool == pool) { fr = i; break; } if (fr < 0 && !g_ar_pool[i].pool) fr = i; }
    if (fr >= 0) { g_ar_pool[fr].pool = st ? pool : NULL; g_ar_pool[fr].st = st; }
    pthread_mutex_unlock(&g_ar);
}
static int ar_pool_get(const void *pool) {
    int st = 3; pthread_mutex_lock(&g_ar);
    for (int i = 0; i < 256; i++) if (g_ar_pool[i].pool == pool) st = g_ar_pool[i].st;
    pthread_mutex_unlock(&g_ar); return st;
}
// 不出声时的时间模型：按真实时间推进各人声 | Time model when there is no output: advance each voice in real time
static void ar_update(void) {
    if (g_ar_real) return;
    pthread_mutex_lock(&g_ar);
    uint64_t now = now_ns(); double dt = g_ar_last ? (now - g_ar_last) / 1e9 : 0; g_ar_last = now; if (dt > 0.25) dt = 0.25;
    for (ar_voice *v = g_ar_voices; v; v = v->next) {
        if (v->state != 0 || !v->nq) continue;
        double adv = dt * v->rate * (v->pitch > 0 ? v->pitch : 1); v->played += (int64_t)adv;
        while (adv > 0 && v->nq) {
            ar_ent *e = &v->q[0]; double len = e->n - ar_start(e), left = len - v->pos;
            if (adv < left) { v->pos += adv; break; }
            adv -= left; v->pos = 0;
            if (e->w->loop) { if (len <= 0) break; continue; }
            ar_pop(v);
        }
    }
    pthread_mutex_unlock(&g_ar);
}
static int ar_voice_acquire(void *key, int rate, int ch, int fmt, const void *par, uint64_t par_size) {
    pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key);
    if (!v) { v = calloc(1, sizeof *v); v->key = key; v->next = g_ar_voices; g_ar_voices = v; }
    ar_flush(v); v->nrel = 0;
    v->rate = rate; v->ch = ch; v->fmt = fmt; v->state = 1; v->pitch = 1; v->pos = 0; v->played = 0; v->dest = NULL; memset(v->mv, 0, sizeof v->mv); memset(v->coef, 0, sizeof v->coef);
    if (fmt == 6 && par && par_size >= sizeof v->coef) memcpy(v->coef, par, sizeof v->coef);
    ALOG("voice %p rate=%d ch=%d fmt=%d par=%llu", key, rate, ch, fmt, (unsigned long long)par_size);
    pthread_mutex_unlock(&g_ar); return 1;
}
static void ar_voice_release(void *key) {
    pthread_mutex_lock(&g_ar);
    for (ar_voice **pp = &g_ar_voices; *pp; pp = &(*pp)->next) if ((*pp)->key == key) { ar_voice *v = *pp; *pp = v->next; while (v->nq) { ar_ent_drop(&v->q[0]); memmove(v->q, v->q + 1, sizeof v->q[0] * (size_t)--v->nq); } free(v); break; }
    pthread_mutex_unlock(&g_ar);
}
static int ar_voice_valid(const void *key) { pthread_mutex_lock(&g_ar); int r = ar_find(key) != NULL; pthread_mutex_unlock(&g_ar); return r; }
static void ar_voice_state(void *key, int st) { pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); if (v) { v->state = st; if (st == 1) ar_flush(v); } pthread_mutex_unlock(&g_ar); }   // 1 = 停止：挂着的缓冲全部归还 | 1 = stop: every queued buffer is returned
static int ar_voice_getstate(const void *key) { pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); int r = v ? v->state : 1; pthread_mutex_unlock(&g_ar); return r; }
static void ar_voice_pitch(void *key, float p) { pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); if (v) v->pitch = p; pthread_mutex_unlock(&g_ar); }
static void ar_voice_dest(void *key, const void *mix) { ALOG("voice %p → mix %p", key, mix); pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); if (v) v->dest = mix; pthread_mutex_unlock(&g_ar); }
static void ar_voice_mixvol(void *key, const void *mix, float vol, int s, int d) {
    ALOG("voice %p mixvol → %p [%d→%d] = %g", key, mix, s, d, vol);
    pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key);
    if (v && s >= 0 && s < AR_VCH && d >= 0 && d < AR_NB) { if (!v->dest) v->dest = mix; if (v->dest == mix) v->mv[s][d] = vol; }
    pthread_mutex_unlock(&g_ar);
}
static int ar_voice_append(void *key, const void *wb) {
    { const ar_wave *w = wb; ALOG("append %p buf=%p size=%llu start=%d end=%d loop=%d ctx=%llu", key, w->buf, (unsigned long long)w->size, w->start, w->end, w->loop, (unsigned long long)w->ctx_size); }
    pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); int ok = v && v->nq + v->nrel < AR_Q;
    if (ok) { ok = ar_ent_make(v, wb, &v->q[v->nq]); if (ok) v->nq++; }
    pthread_mutex_unlock(&g_ar); return ok;
}
static const void *ar_voice_released(void *key) {
    pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); const void *r = NULL;
    if (v && v->nrel) { r = v->rel[0]; memmove(v->rel, v->rel + 1, sizeof v->rel[0] * (size_t)--v->nrel); }
    pthread_mutex_unlock(&g_ar); return r;
}
static int ar_voice_count(const void *key) { pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); int r = v ? v->nq + v->nrel : 0; pthread_mutex_unlock(&g_ar); return r; }
static int64_t ar_voice_played(const void *key) { pthread_mutex_lock(&g_ar); ar_voice *v = ar_find(key); int64_t r = v ? v->played : 0; pthread_mutex_unlock(&g_ar); return r; }
static int ar_mix_acquire(void *key, int nbuf, int is_final) {
    ALOG("%s %p 路数 %d", is_final ? "finalmix" : "submix", key, nbuf);
    pthread_mutex_lock(&g_ar); ar_mix *m = ar_mix_find(key);
    if (!m) { m = calloc(1, sizeof *m); m->acc = calloc(AR_NB * AR_MAXF, sizeof(float)); m->key = key; m->next = g_ar_mixes; g_ar_mixes = m; }
    m->nbuf = nbuf; m->is_final = is_final; m->vol = 1; m->dest = NULL; memset(m->mv, 0, sizeof m->mv);
    pthread_mutex_unlock(&g_ar); return 1;
}
static void ar_mix_release(void *key) {
    pthread_mutex_lock(&g_ar);
    for (ar_mix **pp = &g_ar_mixes; *pp; pp = &(*pp)->next) if ((*pp)->key == key) { ar_mix *m = *pp; *pp = m->next; free(m->acc); free(m); break; }
    pthread_mutex_unlock(&g_ar);
}
static void ar_mix_dest(void *key, const void *dest) { ALOG("mix %p → %p", key, dest); pthread_mutex_lock(&g_ar); ar_mix *m = ar_mix_find(key); if (m) m->dest = dest; pthread_mutex_unlock(&g_ar); }
static void ar_mix_vol(void *key, float vol) { ALOG("mix %p vol = %g", key, vol); pthread_mutex_lock(&g_ar); ar_mix *m = ar_mix_find(key); if (m) m->vol = vol; pthread_mutex_unlock(&g_ar); }
static void ar_mix_mixvol(void *key, const void *dest, float vol, int s, int d) {
    ALOG("mix %p mixvol → %p [%d→%d] = %g", key, dest, s, d, vol);
    pthread_mutex_lock(&g_ar); ar_mix *m = ar_mix_find(key);
    if (m && s >= 0 && s < AR_NB && d >= 0 && d < AR_NB) { if (!m->dest) m->dest = dest; if (m->dest == dest) m->mv[s][d] = vol; }
    pthread_mutex_unlock(&g_ar);
}

HLE_FORCE("_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm", "?音频渲染器：宿主混音", int, h_ar_AcquireMemoryPool, void *c, void *pool) { (void)c; ar_pool_set(pool, 3); return 1; }
HLE_FORCE("_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE", "?音频渲染器：宿主混音", int, h_ar_RequestAttachMemoryPool, void *pool) { ar_pool_set(pool, 1); return 1; }
HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "?音频渲染器：宿主混音", Result, h_ar_RequestUpdateAudioRenderer, uint64_t h0, uint64_t h1, const void *c) { (void)h0; (void)h1; (void)c; ar_update(); return RES_OK; }
HLE_FORCE("_ZN2nn5audio14IsAuxRemovableEPKNS0_7AuxTypeE", "?音频渲染器：宿主混音", int, h_ar_IsAuxRemovable, void) { return 1; }
HLE_FORCE("_ZN2nn5audio16IsDelayRemovableEPKNS0_9DelayTypeE", "?音频渲染器：宿主混音", int, h_ar_IsDelayRemovable, void) { return 1; }
HLE_FORCE("_ZN2nn5audio9RemoveAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_10SubMixTypeE", "?音频渲染器：宿主混音", void *, h_ar_RemoveAux, void *c, void *fx) { (void)c; return objmap_del(fx); }
HLE_FORCE("_ZN2nn5audio11RemoveDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPNS0_10SubMixTypeE", "?音频渲染器：宿主混音", void *, h_ar_RemoveDelay, void *c, void *fx) { (void)c; return objmap_del(fx); }
HLE_FORCE("_ZN2nn5audio13ReleaseSubMixEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeE", "?音频渲染器：宿主混音", void, h_ar_ReleaseSubMix, void *c, void *m) { (void)c; ar_mix_release(m); }
HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "?音频渲染器：宿主混音", uint64_t, h_ar_GetAudioRendererWorkBufferSize, const void *p) { (void)p; return 0x100000; }
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "?音频渲染器：宿主混音", Result, h_ar_OpenAudioRenderer, uint64_t *h, void *ev, const void *p, void *wb, uint64_t n) { (void)p; (void)wb; (void)n; h[0] = 1; h[1] = 0; ar_open(ev); return RES_OK; }
HLE_FORCE("_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE", "?音频渲染器：宿主混音", uint64_t, h_ar_GetAudioRendererConfigWorkBufferSize, const void *p) { (void)p; return 0x1000; }
HLE_FORCE("_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm", "?音频渲染器：宿主混音", void, h_ar_InitializeAudioRendererConfig, void *c, const void *p, void *b, uint64_t n) { (void)c; (void)p; (void)b; (void)n; }
HLE_FORCE("_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi", "?音频渲染器：宿主混音", int, h_ar_AcquireFinalMix, void *c, void *m, int nbuf) { (void)c; return ar_mix_acquire(m, nbuf, 1); }
HLE_FORCE("_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc", "?音频渲染器：宿主混音", Result, h_ar_AddDeviceSink, void) { return RES_OK; }
HLE_FORCE("_ZN2nn5audio13AcquireSubMixEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEii", "?音频渲染器：宿主混音", int, h_ar_AcquireSubMix, void *c, void *m, int rate, int nbuf) { (void)c; (void)rate; return ar_mix_acquire(m, nbuf, 0); }
HLE_FORCE("_ZN2nn5audio20SetSubMixDestinationEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEPNS0_12FinalMixTypeE", "?音频渲染器：宿主混音", void, h_ar_SetSubMixDestination, void *c, void *m, const void *dest) { (void)c; ar_mix_dest(m, dest); }
HLE_FORCE("_ZN2nn5audio18SetSubMixMixVolumeEPNS0_10SubMixTypeEPNS0_12FinalMixTypeEfii", "?音频渲染器：宿主混音", void, h_ar_SetSubMixMixVolume, void *m, const void *dest, float vol, int s, int d) { ar_mix_mixvol(m, dest, vol, s, d); }
HLE_FORCE("_ZN2nn5audio35GetRequiredBufferSizeForI3dl2ReverbEii", "?音频渲染器：宿主混音", uint64_t, h_ar_GetRequiredBufferSizeForI3dl2Reverb, void) { return 0x10000; }
HLE_FORCE("_ZN2nn5audio14AddI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPvmPNS0_10SubMixTypeEi", "?音频渲染器：宿主混音", Result, h_ar_AddI3dl2Reverb, void *c, void *fx, void *buf) { (void)c; objmap_put(fx, buf); return RES_OK; }
HLE_FORCE("_ZN2nn5audio25SetI3dl2ReverbInputOutputEPNS0_15I3dl2ReverbTypeEPKaS4_i", "?音频渲染器：宿主混音", void, h_ar_SetI3dl2ReverbInputOutput, void) {  }
HLE_FORCE("_ZN2nn5audio21SetI3dl2ReverbEnabledEPNS0_15I3dl2ReverbTypeEb", "?音频渲染器：宿主混音", void, h_ar_SetI3dl2ReverbEnabled, void) {  }
HLE_FORCE("_ZN2nn5audio29GetRequiredBufferSizeForDelayENS_8TimeSpanEii", "?音频渲染器：宿主混音", uint64_t, h_ar_GetRequiredBufferSizeForDelay, void) { return 0x40000; }
HLE_FORCE("_ZN2nn5audio8AddDelayEPNS0_19AudioRendererConfigEPNS0_9DelayTypeEPvmPNS0_10SubMixTypeENS_8TimeSpanEi", "?音频渲染器：宿主混音", Result, h_ar_AddDelay, void *c, void *fx, void *buf) { (void)c; objmap_put(fx, buf); return RES_OK; }
HLE_FORCE("_ZN2nn5audio19SetDelayInputOutputEPNS0_9DelayTypeEPKaS4_i", "?音频渲染器：宿主混音", void, h_ar_SetDelayInputOutput, void) {  }
HLE_FORCE("_ZN2nn5audio12SetDelayTimeEPNS0_9DelayTypeENS_8TimeSpanE", "?音频渲染器：宿主混音", void, h_ar_SetDelayTime, void) {  }
HLE_FORCE("_ZN2nn5audio14SetDelayInGainEPNS0_9DelayTypeEf", "?音频渲染器：宿主混音", void, h_ar_SetDelayInGain, void) {  }
HLE_FORCE("_ZN2nn5audio15SetDelayDryGainEPNS0_9DelayTypeEf", "?音频渲染器：宿主混音", void, h_ar_SetDelayDryGain, void) {  }
HLE_FORCE("_ZN2nn5audio15SetDelayEnabledEPNS0_9DelayTypeEb", "?音频渲染器：宿主混音", void, h_ar_SetDelayEnabled, void) {  }
HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "?音频渲染器：宿主混音", Result, h_ar_StartAudioRenderer, uint64_t h0, uint64_t h1) { (void)h0; (void)h1; ar_startup(); LOG("audio：渲染器启动"); return RES_OK; }
HLE_FORCE("_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE", "?音频渲染器：宿主混音", int, h_ar_RequestDetachMemoryPool, void *pool) { ar_pool_set(pool, 3); return 1; }
HLE_FORCE("_ZN2nn5audio24SetI3dl2ReverbParametersEPNS0_15I3dl2ReverbTypeEPKNS0_23I3dl2ReverbParameterSetE", "?音频渲染器：宿主混音", void, h_ar_SetI3dl2ReverbParameters, void) {  }
HLE_FORCE("_ZN2nn5audio20SetDelayFeedbackGainEPNS0_9DelayTypeEf", "?音频渲染器：宿主混音", void, h_ar_SetDelayFeedbackGain, void) {  }
HLE_FORCE("_ZN2nn5audio21SetDelayLowPassAmountEPNS0_9DelayTypeEf", "?音频渲染器：宿主混音", void, h_ar_SetDelayLowPassAmount, void) {  }
HLE_FORCE("_ZN2nn5audio20IsMemoryPoolAttachedEPKNS0_14MemoryPoolTypeE", "?音频渲染器：宿主混音", int, h_ar_IsMemoryPoolAttached, const void *pool) { return ar_pool_get(pool) == 1; }
HLE_FORCE("_ZN2nn5audio17ReleaseMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeE", "?音频渲染器：宿主混音", void, h_ar_ReleaseMemoryPool, void *c, void *pool) { (void)c; ar_pool_set(pool, 0); }
HLE_FORCE("_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE", "?音频渲染器：宿主混音", void, h_ar_SetVoicePlayState, void *v, int st) { ar_voice_state(v, st); }
HLE_FORCE("_ZN2nn5audio18CopyMemoryPoolDataEPKNS0_14MemoryPoolTypeEPvPKvm", "?音频渲染器：宿主混音", void, h_ar_CopyMemoryPoolData, const void *pool, void *dst, const void *src, uint64_t n) { (void)pool; memcpy(dst, src, n); }
HLE_FORCE("_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE", "?音频渲染器：宿主混音", int, h_ar_AppendWaveBuffer, void *v, const void *wb) { return ar_voice_append(v, wb); }
HLE_FORCE("_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE", "?音频渲染器：宿主混音", const void *, h_ar_GetReleasedWaveBuffer, void *v) { return ar_voice_released(v); }
HLE_FORCE("_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvm", "?音频渲染器：宿主混音", int, h_ar_AcquireVoiceSlot, void *c, void *v, int rate, int ch, int fmt, int prio, const void *par, uint64_t par_size) { (void)c; (void)prio; return ar_voice_acquire(v, rate, ch, fmt, par, par_size); }
HLE_FORCE("_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_10SubMixTypeE", "?音频渲染器：宿主混音", void, h_ar_SetVoiceDestination, void *c, void *v, const void *mix) { (void)c; ar_voice_dest(v, mix); }
HLE_FORCE("_ZN2nn5audio13SetVoicePitchEPNS0_9VoiceTypeEf", "?音频渲染器：宿主混音", void, h_ar_SetVoicePitch, void *v, float pitch) { ar_voice_pitch(v, pitch); }
HLE_FORCE("_ZN2nn5audio17GetVoicePlayStateEPKNS0_9VoiceTypeE", "?音频渲染器：宿主混音", int, h_ar_GetVoicePlayState, const void *v) { return ar_voice_getstate(v); }
HLE_FORCE("_ZN2nn5audio13SetAuxEnabledEPNS0_7AuxTypeEb", "?音频渲染器：宿主混音", void, h_ar_SetAuxEnabled, void) {  }
HLE_FORCE("_ZN2nn5audio17GetAuxSampleCountEPKNS0_7AuxTypeE", "?音频渲染器：宿主混音", int, h_ar_GetAuxSampleCount, void) { return 0; }
HLE_FORCE("_ZN2nn5audio17ReadAuxSendBufferEPNS0_7AuxTypeEPii", "?音频渲染器：宿主混音", int, h_ar_ReadAuxSendBuffer, void) { return 0; }
HLE_FORCE("_ZN2nn5audio20WriteAuxReturnBufferEPNS0_7AuxTypeEPKii", "?音频渲染器：宿主混音", int, h_ar_WriteAuxReturnBuffer, void *aux, const int *buf, int n) { (void)aux; (void)buf; return n; }
HLE_FORCE("_ZN2nn5audio12IsVoiceValidEPKNS0_9VoiceTypeE", "?音频渲染器：宿主混音", int, h_ar_IsVoiceValid, const void *v) { return ar_voice_valid(v); }
HLE_FORCE("_ZN2nn5audio25GetVoicePlayedSampleCountEPKNS0_9VoiceTypeE", "?音频渲染器：宿主混音", int64_t, h_ar_GetVoicePlayedSampleCount, const void *v) { return ar_voice_played(v); }
HLE_FORCE("_ZN2nn5audio16ReleaseVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeE", "?音频渲染器：宿主混音", void, h_ar_ReleaseVoiceSlot, void *c, void *v) { (void)c; ar_voice_release(v); }
HLE_FORCE("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_10SubMixTypeEfii", "?音频渲染器：宿主混音", void, h_ar_SetVoiceMixVolume, void *v, const void *mix, float vol, int s, int d) { ar_voice_mixvol(v, mix, vol, s, d); }
HLE_FORCE("_ZN2nn5audio18GetWaveBufferCountEPKNS0_9VoiceTypeE", "?音频渲染器：宿主混音", int, h_ar_GetWaveBufferCount, const void *v) { return ar_voice_count(v); }
HLE_FORCE("_ZN2nn5audio12GetDelayTimeEPKNS0_9DelayTypeE", "?音频渲染器：宿主混音", int64_t, h_ar_GetDelayTime, void) { return 0; }
HLE_FORCE("_ZN2nn5audio43GetRequiredBufferSizeForAuxSendReturnBufferEPKNS0_22AudioRendererParameterEii", "?音频渲染器：宿主混音", uint64_t, h_ar_GetRequiredBufferSizeForAuxSendReturnBuffer, void) { return 0x10000; }
HLE_FORCE("_ZN2nn5audio6AddAuxEPNS0_19AudioRendererConfigEPNS0_7AuxTypeEPNS0_10SubMixTypeEPvS7_m", "?音频渲染器：宿主混音", Result, h_ar_AddAux, void *c, void *fx, void *sub, void *buf) { (void)c; (void)sub; objmap_put(fx, buf); return RES_OK; }
HLE_FORCE("_ZN2nn5audio17SetAuxInputOutputEPNS0_7AuxTypeEPKaS4_i", "?音频渲染器：宿主混音", void, h_ar_SetAuxInputOutput, void) {  }
HLE_FORCE("_ZN2nn5audio15SetSubMixVolumeEPNS0_10SubMixTypeEf", "?音频渲染器：宿主混音", void, h_ar_SetSubMixVolume, void *m, float vol) { ar_mix_vol(m, vol); }
HLE_FORCE("_ZN2nn5audio20SetSubMixDestinationEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeES4_", "?音频渲染器：宿主混音", void, h_ar_SetSubMixDestination1, void *c, void *m, const void *dest) { (void)c; ar_mix_dest(m, dest); }
HLE_FORCE("_ZN2nn5audio18SetSubMixMixVolumeEPNS0_10SubMixTypeES2_fii", "?音频渲染器：宿主混音", void, h_ar_SetSubMixMixVolume1, void *m, const void *dest, float vol, int s, int d) { ar_mix_mixvol(m, dest, vol, s, d); }
HLE_FORCE("_ZN2nn5audio18GetMemoryPoolStateEPKNS0_14MemoryPoolTypeE", "?音频渲染器：宿主混音", int, h_ar_GetMemoryPoolState, const void *pool) { return ar_pool_get(pool); }
