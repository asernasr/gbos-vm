/*
 * Apple Silicon (MacBookAir10,1 / j313) Speaker Protection & Audio DSP Daemon
 *
 * Implements the two mandatory audio protection layers required on bare-metal
 * Apple Silicon Macs (snd-soc-macaudio + TI TAS2764 smart amps + Cirrus CS42L83):
 *
 *  1. Real-Time V/I-Sense Thermal Excursion Protection (speakersafetyd):
 *     - Monitors voltage/current sense feedback from the left and right TAS2764
 *       amplifiers (or estimates conservative upper-bound coil power from post-DSP
 *       drive level if IV-sense PCM is unavailable during early init).
 *     - Integrates a dual-stage voice-coil + magnet thermal model (T_coil, T_mag)
 *       tuned for the j313 speaker assembly.
 *     - Periodically strobes the snd-soc-macaudio kernel safety lock control
 *       ("Speaker Driver" / "Speaker Volume" / "Speaker Gain Limit") only while
 *       voice-coil temperature delta is within safe limits (< 75 C rise), and
 *       immediately mutes/locks the amps on over-temperature or fault.
 *
 *  2. Real-Time Speaker DSP Filter & Lookahead Limiter Chain:
 *     - Captures Android's primary PCM output from ALSA Loopback (snd-aloop card 0,
 *       device 1 capture) and routes it to the AppleJ313 hardware card:
 *       * Internal Speakers (TAS2764):
 *         - 4th-order Linkwitz-Riley (24 dB/oct) High-Pass Filter at 125 Hz
 *           (prevents mechanical cone over-excursion below enclosure resonance).
 *         - Bankstown-style psychoacoustic bass harmonic generator (45-125 Hz ->
 *           2nd/3rd harmonics above 130 Hz).
 *         - 4-band parametric biquad EQ matched to the j313 acoustic profile.
 *         - 5 ms lookahead brickwall peak/RMS limiter (-1.5 dBFS ceiling scaled
 *           by the real-time voice-coil thermal gain factor).
 *       * 3.5mm Headphone Jack (CS42L83):
 *         - Bypasses speaker HPF/EQ and applies clean headroom limiting only.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>
#include <android/log.h>
#include <sys/system_properties.h>

#define LOG_TAG "AsahiSpeakerSafetyDSP"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SAMPLE_RATE         48000U
#define CHANNELS            2U
#define PERIOD_FRAMES       240U    /* 5.0 ms period at 48 kHz */
#define PERIOD_COUNT        4U
#define LOOKAHEAD_FRAMES    240U    /* 5.0 ms brickwall lookahead */

/* j313 (MacBookAir10,1) TAS2764 thermal & excursion constants */
#define J313_R_COIL_AMP     14.5f   /* K / W (voice coil to magnet thermal resistance) */
#define J313_TAU_COIL_SEC   0.18f   /* Voice coil thermal time constant (s) */
#define J313_R_MAG_AMB      8.2f    /* K / W (magnet to chassis ambient resistance) */
#define J313_TAU_MAG_SEC    12.0f   /* Magnet thermal time constant (s) */
#define J313_NOMINAL_OHMS   6.8f    /* Nominal speaker DC impedance */
#define J313_MAX_WATTS      3.5f    /* Full-scale TAS2764 power into 6.8 ohms */
#define J313_DT_WARN_C      42.0f   /* Start soft thermal gain reduction (+42 C) */
#define J313_DT_LIMIT_C     65.0f   /* Aggressive attenuation ceiling (+65 C) */
#define J313_DT_PANIC_C     75.0f   /* Emergency hard-mute & safety lock drop (+75 C) */
#define CEILING_DBFS_LIN    0.841395f /* -1.5 dBFS hard brickwall ceiling */

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* Direct-Form II Transposed Biquad Filter */
typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1[CHANNELS];
    float z2[CHANNELS];
} biquad_t;

static inline float biquad_step(biquad_t *f, int ch, float x) {
    float y = f->b0 * x + f->z1[ch];
    f->z1[ch] = f->b1 * x - f->a1 * y + f->z2[ch];
    f->z2[ch] = f->b2 * x - f->a2 * y;
    return y;
}

static void biquad_init_hpf(biquad_t *f, float fc, float q, float fs) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * q);
    float a0 = 1.0f + alpha;
    f->b0 = ((1.0f + cos_w0) * 0.5f) / a0;
    f->b1 = -(1.0f + cos_w0) / a0;
    f->b2 = ((1.0f + cos_w0) * 0.5f) / a0;
    f->a1 = (-2.0f * cos_w0) / a0;
    f->a2 = (1.0f - alpha) / a0;
    memset(f->z1, 0, sizeof(f->z1));
    memset(f->z2, 0, sizeof(f->z2));
}

static void biquad_init_bpf(biquad_t *f, float fc, float q, float fs) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * q);
    float a0 = 1.0f + alpha;
    f->b0 = alpha / a0;
    f->b1 = 0.0f;
    f->b2 = -alpha / a0;
    f->a1 = (-2.0f * cos_w0) / a0;
    f->a2 = (1.0f - alpha) / a0;
    memset(f->z1, 0, sizeof(f->z1));
    memset(f->z2, 0, sizeof(f->z2));
}

static void biquad_init_peaking(biquad_t *f, float fc, float q, float db_gain, float fs) {
    float A = powf(10.0f, db_gain / 40.0f);
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * q);
    float a0 = 1.0f + alpha / A;
    f->b0 = (1.0f + alpha * A) / a0;
    f->b1 = (-2.0f * cos_w0) / a0;
    f->b2 = (1.0f - alpha * A) / a0;
    f->a1 = (-2.0f * cos_w0) / a0;
    f->a2 = (1.0f - alpha / A) / a0;
    memset(f->z1, 0, sizeof(f->z1));
    memset(f->z2, 0, sizeof(f->z2));
}

static void biquad_init_highshelf(biquad_t *f, float fc, float q, float db_gain, float fs) {
    float A = powf(10.0f, db_gain / 40.0f);
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * q);
    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;
    float a0 = (A + 1.0f) - (A - 1.0f) * cos_w0 + two_sqrtA_alpha;
    f->b0 = (A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + two_sqrtA_alpha)) / a0;
    f->b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0)) / a0;
    f->b2 = (A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - two_sqrtA_alpha)) / a0;
    f->a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0)) / a0;
    f->a2 = ((A + 1.0f) - (A - 1.0f) * cos_w0 - two_sqrtA_alpha) / a0;
    memset(f->z1, 0, sizeof(f->z1));
    memset(f->z2, 0, sizeof(f->z2));
}

typedef struct {
    /* Stage 1: 4th-order Linkwitz-Riley (24 dB/oct) HPF at 125 Hz */
    biquad_t hpf_stage1;
    biquad_t hpf_stage2;
    /* Stage 2: Bankstown sub-bass harmonic generator (45-125 Hz -> >130 Hz) */
    biquad_t bass_extract_bpf;
    biquad_t bass_harm_hpf;
    /* Stage 3: j313 MacBook Air 4-band enclosure compensation EQ */
    biquad_t eq_band[4];
    /* Stage 4: 5 ms lookahead brickwall limiter */
    float lookahead_buf[LOOKAHEAD_FRAMES][CHANNELS];
    uint32_t lookahead_idx;
    float env_gain;
    /* Real-time TAS2764 dual-stage speaker thermal state (per channel) */
    float dt_coil[CHANNELS];
    float dt_mag[CHANNELS];
    float thermal_gain;
    bool emergency_muted;
} j313_dsp_state_t;

static void j313_dsp_init(j313_dsp_state_t *s) {
    memset(s, 0, sizeof(*s));
    const float fs = (float)SAMPLE_RATE;
    /* Two cascaded Butterworth HPFs (Q = 0.7071) = 4th-order Linkwitz-Riley 24 dB/oct at 125 Hz */
    biquad_init_hpf(&s->hpf_stage1, 125.0f, 0.70710678f, fs);
    biquad_init_hpf(&s->hpf_stage2, 125.0f, 0.70710678f, fs);

    /* Psychoacoustic bass harmonic generator */
    biquad_init_bpf(&s->bass_extract_bpf, 80.0f, 1.2f, fs);
    biquad_init_hpf(&s->bass_harm_hpf, 135.0f, 0.70710678f, fs);

    /* j313 (MacBookAir10,1) acoustic correction biquads */
    biquad_init_peaking(&s->eq_band[0], 950.0f,  2.2f, -4.2f, fs);
    biquad_init_peaking(&s->eq_band[1], 2450.0f, 2.8f, -3.5f, fs);
    biquad_init_peaking(&s->eq_band[2], 4800.0f, 1.8f, +2.0f, fs);
    biquad_init_highshelf(&s->eq_band[3], 10500.0f, 0.707f, +2.5f, fs);

    s->env_gain = 1.0f;
    s->thermal_gain = 1.0f;
    s->emergency_muted = false;
}

/*
 * Update dual-stage voice-coil + magnet thermal model using either measured
 * TAS2764 V/I-sense capture samples or conservative post-limiter output power.
 */
static void j313_update_thermal_model(j313_dsp_state_t *s,
                                      const int16_t *iv_sense_frames,
                                      size_t frame_count,
                                      const float post_dsp_rms[CHANNELS],
                                      float dt_sec) {
    float power_w[CHANNELS] = {0.0f, 0.0f};

    if (iv_sense_frames && frame_count > 0) {
        /*
         * TAS2764 4-slot TDM capture frame: [V_L, I_L, V_R, I_R]
         * Full-scale V_sense = 14.2 Vp, Full-scale I_sense = 3.5 Ap
         */
        double sum_p_l = 0.0, sum_p_r = 0.0;
        for (size_t i = 0; i < frame_count; i++) {
            float v_l = ((float)iv_sense_frames[i * 4 + 0] / 32768.0f) * 14.2f;
            float i_l = ((float)iv_sense_frames[i * 4 + 1] / 32768.0f) * 3.5f;
            float v_r = ((float)iv_sense_frames[i * 4 + 2] / 32768.0f) * 14.2f;
            float i_r = ((float)iv_sense_frames[i * 4 + 3] / 32768.0f) * 3.5f;
            sum_p_l += fabsf(v_l * i_l);
            sum_p_r += fabsf(v_r * i_r);
        }
        power_w[0] = (float)(sum_p_l / (double)frame_count);
        power_w[1] = (float)(sum_p_r / (double)frame_count);
    } else {
        /* Conservative electrical power estimate from post-limiter RMS drive */
        for (int c = 0; c < (int)CHANNELS; c++) {
            power_w[c] = (post_dsp_rms[c] * post_dsp_rms[c]) * J313_MAX_WATTS;
        }
    }

    float max_dt = 0.0f;
    float alpha_coil = dt_sec / (J313_TAU_COIL_SEC + dt_sec);
    float alpha_mag  = dt_sec / (J313_TAU_MAG_SEC + dt_sec);

    for (int c = 0; c < (int)CHANNELS; c++) {
        float target_coil_above_mag = power_w[c] * J313_R_COIL_AMP;
        float target_mag_above_amb  = power_w[c] * J313_R_MAG_AMB;

        float coil_above_mag = (s->dt_coil[c] - s->dt_mag[c]);
        coil_above_mag += alpha_coil * (target_coil_above_mag - coil_above_mag);
        s->dt_mag[c] += alpha_mag * (target_mag_above_amb - s->dt_mag[c]);
        s->dt_coil[c] = s->dt_mag[c] + coil_above_mag;

        if (s->dt_coil[c] > max_dt) {
            max_dt = s->dt_coil[c];
        }
    }

    if (max_dt >= J313_DT_PANIC_C) {
        s->emergency_muted = true;
        s->thermal_gain = 0.0f;
        ALOGE("THERMAL PROTECTION TRIPPED: max voice-coil dT=%.1f C >= %.1f C; muting speakers!",
              max_dt, J313_DT_PANIC_C);
    } else if (max_dt >= J313_DT_WARN_C) {
        if (s->emergency_muted && max_dt < (J313_DT_WARN_C - 5.0f)) {
            s->emergency_muted = false;
        }
        float frac = (max_dt - J313_DT_WARN_C) / (J313_DT_LIMIT_C - J313_DT_WARN_C);
        if (frac > 1.0f) frac = 1.0f;
        /* Attenuate up to -12 dB (0.25 linear) as coil approaches limit */
        s->thermal_gain = s->emergency_muted ? 0.0f : (1.0f - 0.75f * frac);
    } else {
        s->emergency_muted = false;
        s->thermal_gain = 1.0f;
    }
}

static void j313_process_period(j313_dsp_state_t *s,
                                const int16_t *in_pcm,
                                int16_t *out_pcm,
                                size_t frames,
                                bool is_headphones,
                                const int16_t *iv_sense_pcm) {
    const float release_coeff = expf(-1.0f / (0.060f * (float)SAMPLE_RATE));
    const float ceiling = is_headphones ? 0.96f : (CEILING_DBFS_LIN * s->thermal_gain);
    double sum_sq[CHANNELS] = {0.0, 0.0};

    for (size_t i = 0; i < frames; i++) {
        float x[CHANNELS];
        for (int c = 0; c < (int)CHANNELS; c++) {
            x[c] = (float)in_pcm[i * CHANNELS + c] / 32768.0f;
        }

        if (!is_headphones) {
            for (int c = 0; c < (int)CHANNELS; c++) {
                /* Extract sub-bass before HPF and generate 2nd/3rd harmonics (Bankstown) */
                float sub = biquad_step(&s->bass_extract_bpf, c, x[c]);
                float harm = tanhf(sub * 2.8f) - sub;
                harm = biquad_step(&s->bass_harm_hpf, c, harm) * 0.35f;

                /* 4th-order Linkwitz-Riley 125 Hz High-Pass Excursion Protection */
                float hp = biquad_step(&s->hpf_stage1, c, x[c]);
                hp = biquad_step(&s->hpf_stage2, c, hp);

                float y = hp + harm;
                /* 4-band j313 speaker enclosure EQ */
                for (int b = 0; b < 4; b++) {
                    y = biquad_step(&s->eq_band[b], c, y);
                }
                x[c] = y;
            }
        }

        /* Peak detection for lookahead brickwall limiter */
        float peak = fmaxf(fabsf(x[0]), fabsf(x[1]));
        float target_gain = 1.0f;
        if (peak * s->env_gain > ceiling && peak > 1e-6f) {
            target_gain = ceiling / peak;
        }
        if (target_gain < s->env_gain) {
            /* Instantaneous brickwall attack */
            s->env_gain = target_gain;
        } else {
            /* Smooth 60 ms release */
            s->env_gain = release_coeff * s->env_gain + (1.0f - release_coeff) * fminf(target_gain, 1.0f);
        }

        /* Pop oldest frame from 5 ms lookahead line and apply brickwall gain */
        uint32_t idx = s->lookahead_idx;
        for (int c = 0; c < (int)CHANNELS; c++) {
            float delayed = s->lookahead_buf[idx][c];
            s->lookahead_buf[idx][c] = x[c];
            float out_f = delayed * s->env_gain;
            if (out_f > ceiling) out_f = ceiling;
            if (out_f < -ceiling) out_f = -ceiling;
            sum_sq[c] += (double)(out_f * out_f);
            int32_t pcm_val = (int32_t)lrintf(out_f * 32767.0f);
            if (pcm_val > 32767) pcm_val = 32767;
            if (pcm_val < -32768) pcm_val = -32768;
            out_pcm[i * CHANNELS + c] = (int16_t)pcm_val;
        }
        s->lookahead_idx = (idx + 1U) % LOOKAHEAD_FRAMES;
    }

    float rms[CHANNELS] = {
        sqrtf((float)(sum_sq[0] / (double)frames)),
        sqrtf((float)(sum_sq[1] / (double)frames)),
    };
    j313_update_thermal_model(s, iv_sense_pcm, frames, rms,
                              (float)frames / (float)SAMPLE_RATE);
}

/* Write integer/boolean ALSA control by name on /dev/snd/controlC<card> */
static int alsa_ctl_set_by_name(int ctl_fd, const char *name, long val) {
    if (ctl_fd < 0) return -1;
    struct snd_ctl_elem_value ev;
    memset(&ev, 0, sizeof(ev));
    ev.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy((char *)ev.id.name, name, sizeof(ev.id.name) - 1);
    ev.value.integer.value[0] = val;
    ev.value.integer.value[1] = val;
    return ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
}

/*
 * Configure AppleJ313 (snd-soc-macaudio + tas2764 + cs42l83) mixer controls and
 * strobe the kernel speaker safety lock control.
 */
static void apple_j313_strobe_speaker_safety(int ctl_fd, const j313_dsp_state_t *s) {
    if (ctl_fd < 0) return;
    if (s->emergency_muted) {
        alsa_ctl_set_by_name(ctl_fd, "Speaker Driver", 0);
        alsa_ctl_set_by_name(ctl_fd, "Left Speaker Playback Switch", 0);
        alsa_ctl_set_by_name(ctl_fd, "Right Speaker Playback Switch", 0);
        return;
    }
    /* Enable V/I-sense telemetry feedback on Left and Right TAS2764 amps */
    alsa_ctl_set_by_name(ctl_fd, "Left VSENSE Switch", 1);
    alsa_ctl_set_by_name(ctl_fd, "Left ISENSE Switch", 1);
    alsa_ctl_set_by_name(ctl_fd, "Right VSENSE Switch", 1);
    alsa_ctl_set_by_name(ctl_fd, "Right ISENSE Switch", 1);
    alsa_ctl_set_by_name(ctl_fd, "Left Speaker Playback Switch", 1);
    alsa_ctl_set_by_name(ctl_fd, "Right Speaker Playback Switch", 1);
    /* Strobe snd-soc-macaudio speaker safety watchdog unlock */
    alsa_ctl_set_by_name(ctl_fd, "Speaker Driver", 1);
}

/* Discover ALSA card index by substring in /proc/asound/cards */
static int find_alsa_card(const char *needle) {
    FILE *fp = fopen("/proc/asound/cards", "r");
    if (!fp) return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        int idx = -1;
        if (sscanf(line, " %d [", &idx) == 1 && strstr(line, needle) != NULL) {
            found = idx;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* Configure a 16-bit interleaved ALSA PCM device via standard SNDRV_PCM_IOCTL_HW_PARAMS */
static int configure_alsa_pcm(int fd, uint32_t rate, uint32_t channels) {
    struct snd_pcm_hw_params params;
    memset(&params, 0, sizeof(params));
    memset(&params.masks, 0xff, sizeof(params.masks));
    for (int i = 0; i < SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1; i++) {
        params.intervals[i].min = 0;
        params.intervals[i].max = ~0U;
    }
    params.rmask = ~0U;
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0) {
        return -1;
    }
    /* Access: RW_INTERLEAVED, Format: S16_LE */
    memset(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0,
           sizeof(struct snd_mask));
    params.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] =
        (1U << SNDRV_PCM_ACCESS_RW_INTERLEAVED);

    memset(&params.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0,
           sizeof(struct snd_mask));
    params.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] =
        (1U << SNDRV_PCM_FORMAT_S16_LE);

    params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = channels;
    params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = channels;
    params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = rate;
    params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = rate;
    params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = PERIOD_FRAMES;
    params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = PERIOD_FRAMES;
    params.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = PERIOD_COUNT;
    params.intervals[SNDRV_PCM_HW_PARAM_PERIODS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = PERIOD_COUNT;

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
        return -1;
    }
    return 0;
}

static int pcm_read_frames(int fd, void *buf, uint32_t frames) {
    struct snd_xferi xfer = {
        .buf = buf,
        .frames = frames,
        .result = 0,
    };
    int r = ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
    if (r < 0 && (errno == EPIPE || errno == ESTRPIPE)) {
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0);
    }
    return r;
}

static int pcm_write_frames(int fd, const void *buf, uint32_t frames) {
    struct snd_xferi xfer = {
        .buf = (void *)buf,
        .frames = frames,
        .result = 0,
    };
    int r = ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
    if (r < 0 && (errno == EPIPE || errno == ESTRPIPE)) {
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0);
    }
    return r;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    /* Elevate to SCHED_FIFO real-time audio priority */
    struct sched_param sp = { .sched_priority = 15 };
    sched_setscheduler(0, SCHED_FIFO, &sp);

    j313_dsp_state_t dsp;
    j313_dsp_init(&dsp);

    ALOGI("Starting Apple Silicon (j313 / TAS2764) Speaker Safety & DSP Limiter Daemon");
    __system_property_set("vendor.asahi.audio.safetyd", "running");

    int mac_card = -1;
    int loop_card = -1;
    int ctl_fd = -1;
    int cap_fd = -1;
    int spk_fd = -1;
    int iv_fd = -1;

    int16_t in_buf[PERIOD_FRAMES * CHANNELS];
    int16_t out_buf[PERIOD_FRAMES * CHANNELS];
    int16_t iv_buf[PERIOD_FRAMES * 4];
    uint64_t period_counter = 0;

    while (g_running) {
        if (mac_card < 0) {
            mac_card = find_alsa_card("Apple");
            if (mac_card < 0) mac_card = find_alsa_card("J313");
            if (mac_card < 0) mac_card = find_alsa_card("Mac");
            if (mac_card >= 0) {
                char path[64];
                snprintf(path, sizeof(path), "/dev/snd/controlC%d", mac_card);
                ctl_fd = open(path, O_RDWR | O_CLOEXEC);
                snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", mac_card);
                spk_fd = open(path, O_RDWR | O_CLOEXEC);
                if (spk_fd >= 0) configure_alsa_pcm(spk_fd, SAMPLE_RATE, CHANNELS);
                snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0c", mac_card);
                iv_fd = open(path, O_RDWR | O_CLOEXEC);
                if (iv_fd >= 0) configure_alsa_pcm(iv_fd, SAMPLE_RATE, 4);
                apple_j313_strobe_speaker_safety(ctl_fd, &dsp);
                ALOGI("Bound to AppleJ313 macaudio card %d (ctl=%d spk=%d iv=%d)",
                      mac_card, ctl_fd, spk_fd, iv_fd);
            }
        }
        if (loop_card < 0) {
            loop_card = find_alsa_card("Loopback");
            if (loop_card >= 0) {
                char path[64];
                snprintf(path, sizeof(path), "/dev/snd/pcmC%dD1c", loop_card);
                cap_fd = open(path, O_RDWR | O_CLOEXEC);
                if (cap_fd >= 0) configure_alsa_pcm(cap_fd, SAMPLE_RATE, CHANNELS);
                ALOGI("Bound to ALSA Loopback capture card %d (fd=%d)", loop_card, cap_fd);
            }
        }

        if (cap_fd >= 0 && spk_fd >= 0) {
            memset(in_buf, 0, sizeof(in_buf));
            if (pcm_read_frames(cap_fd, in_buf, PERIOD_FRAMES) == 0) {
                const int16_t *iv_ptr = NULL;
                if (iv_fd >= 0 && pcm_read_frames(iv_fd, iv_buf, PERIOD_FRAMES) == 0) {
                    iv_ptr = iv_buf;
                }
                char hp_prop[32] = {0};
                __system_property_get("vendor.asahi.audio.headphones", hp_prop);
                bool is_hp = (strcmp(hp_prop, "1") == 0);

                j313_process_period(&dsp, in_buf, out_buf, PERIOD_FRAMES, is_hp, iv_ptr);
                pcm_write_frames(spk_fd, out_buf, PERIOD_FRAMES);

                /* Strobe kernel speaker safety watchdog every ~200 ms (40 periods) */
                if ((++period_counter % 40ULL) == 0ULL) {
                    apple_j313_strobe_speaker_safety(ctl_fd, &dsp);
                    char prop_val[32];
                    snprintf(prop_val, sizeof(prop_val), "%.1f",
                             fmaxf(dsp.dt_coil[0], dsp.dt_coil[1]));
                    __system_property_set("vendor.asahi.audio.coil_dt_c", prop_val);
                }
                continue;
            }
        }

        /* Idle / waiting for hardware cards (e.g. during early boot or in VM fallback) */
        usleep(250000);
    }

    /* Fail-safe: lock/mute speakers on daemon shutdown */
    dsp.emergency_muted = true;
    apple_j313_strobe_speaker_safety(ctl_fd, &dsp);
    if (iv_fd >= 0) close(iv_fd);
    if (spk_fd >= 0) close(spk_fd);
    if (cap_fd >= 0) close(cap_fd);
    if (ctl_fd >= 0) close(ctl_fd);
    return 0;
}
