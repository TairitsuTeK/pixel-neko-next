// kernel/audio.c - 统一音频引擎、DSP 处理器与流媒体解码调度器
#include "audio.h"
#include "cmi8738.h"
#include "wav_dec.h"
#include "flac_dec.h"
#include "mp3_dec.h"

#define DEC_NONE 0
#define DEC_WAV  1
#define DEC_FLAC 2
#define DEC_MP3  3

#define PCM_QUEUE_SIZE  65536 // 64KB 环形队列（~340ms 16-bit 48kHz Stereo）

static audio_state_t g_audio_state = AUDIO_STATE_STOPPED;
static audio_info_t g_audio_info;
static int g_active_dec = DEC_NONE;

static int g_master_volume = 85;
static int g_is_muted = 0;

static short g_pcm_queue[PCM_QUEUE_SIZE / sizeof(short)];
static unsigned int g_queue_read_idx = 0;
static unsigned int g_queue_write_idx = 0;
static unsigned int g_queue_bytes = 0;

// 播放时钟与累计帧计数
static unsigned int g_total_frames_played = 0;
static int g_eof_reached = 0;

// 10 段频谱仪与立体声 VU 表
static unsigned char g_spectrum_bands[10] = {0};
static unsigned char g_vu_l = 0, g_vu_r = 0;

// 7-抽头对称低通 FIR 多相滤波系数（归一化截至频率 0.45 * Fs）
static const short g_fir_coeffs[7] = { -12, 0, 138, 256, 138, 0, -12 };

// Xorshift32 伪随机数发生器（用于 TPDF 三角概率密度抖动）
static inline unsigned int xorshift32(void) {
    static unsigned int seed = 0x12345678;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

// 24-bit PCM 转 16-bit PCM，带 TPDF 三角抖动
static inline short dither_s24_to_s16(int s24) {
    int r1 = (int)(xorshift32() & 0xFF);
    int r2 = (int)(xorshift32() & 0xFF);
    int dither = r1 - r2; // [-255, +255]
    int out = (s24 + dither) >> 8;
    if (out > 32767) return 32767;
    if (out < -32768) return -32768;
    return (short)out;
}

// 纯整型解析 IEEE 754 32-bit Float 转换为 24-bit 有符号整型（完全规避 FPU 与 SSE）
static inline int ieee754_to_s24(unsigned int bits) {
    int sign = (bits & 0x80000000) ? -1 : 1;
    int exp = (int)((bits >> 23) & 0xFF) - 127;
    unsigned int mant = (bits & 0x7FFFFF) | 0x800000;
    if (exp < -23) return 0;
    if (exp >= 0) return sign * 0x7FFFFF; // >= 1.0 软削波限制
    return sign * (int)(mant >> (-exp));
}

static void my_strcpy(char* dest, const char* src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static int my_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

void enable_fpu(void) {
    unsigned int cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // 清除 CR0.EM (禁止软件模拟)
    cr0 |=  (1 << 1); // 置位 CR0.MP (监控协处理器)
    cr0 &= ~(1 << 3); // 清除 CR0.TS (任务已切换标志)
    cr0 |=  (1 << 5); // 置位 CR0.NE (内部 x87 浮点错误报告)
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    // 使用 CPUID 检测是否支持 FXSR 和 SSE
    unsigned int edx = 0;
    __asm__ volatile(
        "pushfl\n"
        "pushfl\n"
        "popl %%eax\n"
        "movl %%eax, %%ecx\n"
        "xorl $0x200000, %%eax\n"
        "pushl %%eax\n"
        "popfl\n"
        "pushfl\n"
        "popl %%eax\n"
        "popfl\n"
        "xorl %%ecx, %%eax\n"
        "jz 1f\n"
        "movl $1, %%eax\n"
        "cpuid\n"
        "1:\n"
        : "=d"(edx) : : "eax", "ebx", "ecx"
    );

    unsigned int cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (edx & (1 << 24)) { // FXSR 支持
        cr4 |= (1 << 9);   // OSFXSR
    }
    if (edx & (1 << 25)) { // SSE 支持
        cr4 |= (1 << 10);  // OSXMMEXCPT
    }
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    __asm__ volatile("fninit");
}

void audio_init(void) {
    enable_fpu();
    g_audio_state = AUDIO_STATE_STOPPED;
    g_active_dec = DEC_NONE;
    g_queue_read_idx = 0;
    g_queue_write_idx = 0;
    g_queue_bytes = 0;
    g_total_frames_played = 0;
    g_eof_reached = 0;
    g_is_muted = 0;
    g_master_volume = 85;

    for (int i = 0; i < 10; i++) g_spectrum_bands[i] = 0;
    g_vu_l = 0;
    g_vu_r = 0;

    // 探测并初始化 C-Media CMI8738 硬件声卡
    cmi8738_init();
    if (cmi8738_is_present()) {
        cmi8738_set_volume(g_master_volume, g_master_volume);
    }
}

int audio_has_hardware(void) {
    return cmi8738_is_present();
}

const char* audio_get_hardware_name(void) {
    return cmi8738_get_name();
}

void audio_flush_queue(void) {
    g_queue_read_idx = 0;
    g_queue_write_idx = 0;
    g_queue_bytes = 0;
}

// 多相滤波与格式转换：将任意采样率、位深、单/双声道样本转为 16-bit 立体声写入环形队列
int audio_feed_raw_pcm(const void* samples, unsigned int sample_count, int src_rate, int src_channels, int src_bits, int src_float) {
    if (!samples || sample_count == 0) return 0;

    int target_rate = g_audio_info.target_rate;
    if (target_rate <= 0) target_rate = 44100;

    int decimation_ratio = 1;
    if (src_rate >= target_rate * 4) {
        decimation_ratio = 4;
    } else if (src_rate >= target_rate * 2) {
        decimation_ratio = 2;
    }

    // -------------------------------------------------------------
    // 极速路径 1：16-bit 立体声 1:1 直通 (44.1k/48k 原生输出，零运算开销)
    // -------------------------------------------------------------
    if (decimation_ratio == 1 && src_bits == 16 && src_channels == 2 && !src_float) {
        const short* src_s16 = (const short*)samples;
        unsigned int space_frames = (PCM_QUEUE_SIZE - g_queue_bytes) / 4;
        unsigned int to_copy = (sample_count < space_frames) ? sample_count : space_frames;
        for (unsigned int i = 0; i < to_copy; i++) {
            unsigned int w_idx = g_queue_write_idx;
            g_pcm_queue[w_idx]     = src_s16[i * 2];
            g_pcm_queue[w_idx + 1] = src_s16[i * 2 + 1];
            g_queue_write_idx = (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
            g_queue_bytes += 4;
        }
        return (int)to_copy;
    }

    // -------------------------------------------------------------
    // 极速路径 2：16-bit 立体声 2:1 降采样 (96kHz -> 48kHz / 88.2kHz -> 44.1kHz)
    // 采用 2 点线性平均平滑抽取，单位增益精确为 1.0，完全消灭削波与乘法消耗
    // -------------------------------------------------------------
    if (decimation_ratio == 2 && src_bits == 16 && src_channels == 2 && !src_float) {
        const short* src_s16 = (const short*)samples;
        unsigned int space_frames = (PCM_QUEUE_SIZE - g_queue_bytes) / 4;
        unsigned int out_frames = sample_count / 2;
        unsigned int to_copy = (out_frames < space_frames) ? out_frames : space_frames;
        for (unsigned int i = 0; i < to_copy; i++) {
            int idx = i * 4;
            short l = (short)(((int)src_s16[idx] + (int)src_s16[idx + 2]) >> 1);
            short r = (short)(((int)src_s16[idx + 1] + (int)src_s16[idx + 3]) >> 1);
            unsigned int w_idx = g_queue_write_idx;
            g_pcm_queue[w_idx]     = l;
            g_pcm_queue[w_idx + 1] = r;
            g_queue_write_idx = (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
            g_queue_bytes += 4;
        }
        return (int)(to_copy * 2);
    }

    // -------------------------------------------------------------
    // 极速路径 3：16-bit 立体声 4:1 降采样 (192kHz -> 48kHz)
    // -------------------------------------------------------------
    if (decimation_ratio == 4 && src_bits == 16 && src_channels == 2 && !src_float) {
        const short* src_s16 = (const short*)samples;
        unsigned int space_frames = (PCM_QUEUE_SIZE - g_queue_bytes) / 4;
        unsigned int out_frames = sample_count / 4;
        unsigned int to_copy = (out_frames < space_frames) ? out_frames : space_frames;
        for (unsigned int i = 0; i < to_copy; i++) {
            int idx = i * 8;
            int sum_l = (int)src_s16[idx] + (int)src_s16[idx + 2] + (int)src_s16[idx + 4] + (int)src_s16[idx + 6];
            int sum_r = (int)src_s16[idx + 1] + (int)src_s16[idx + 3] + (int)src_s16[idx + 5] + (int)src_s16[idx + 7];
            unsigned int w_idx = g_queue_write_idx;
            g_pcm_queue[w_idx]     = (short)(sum_l >> 2);
            g_pcm_queue[w_idx + 1] = (short)(sum_r >> 2);
            g_queue_write_idx = (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
            g_queue_bytes += 4;
        }
        return (int)(to_copy * 4);
    }

    // 通用回退路径（用于非标准单声道、8-bit、24-bit、32-bit Float 等 WAV 数据）
    const unsigned char* src_bytes = (const unsigned char*)samples;
    int bytes_per_sample = (src_bits + 7) / 8;
    int frame_size = bytes_per_sample * src_channels;
    unsigned int produced_frames = 0;

    for (unsigned int i = 0; i < sample_count; i += decimation_ratio) {
        if (g_queue_bytes + 4 > PCM_QUEUE_SIZE) break;

        const unsigned char* p = src_bytes + i * frame_size;
        int s24_l = 0, s24_r = 0;

        if (src_float && src_bits == 32) {
            unsigned int u_l = *(const unsigned int*)p;
            s24_l = ieee754_to_s24(u_l);
            s24_r = (src_channels > 1) ? ieee754_to_s24(*(const unsigned int*)(p + 4)) : s24_l;
        } else if (src_bits == 16) {
            s24_l = *(const short*)p << 8;
            s24_r = (src_channels > 1) ? (*(const short*)(p + 2) << 8) : s24_l;
        } else if (src_bits == 24) {
            s24_l = (int)(((unsigned int)p[0]) | ((unsigned int)p[1] << 8) | ((unsigned int)(signed char)p[2] << 16));
            s24_r = (src_channels > 1) ? (int)(((unsigned int)p[3]) | ((unsigned int)p[4] << 8) | ((unsigned int)(signed char)p[5] << 16)) : s24_l;
        } else if (src_bits == 32) {
            s24_l = *(const int*)p >> 8;
            s24_r = (src_channels > 1) ? (*(const int*)(p + 4) >> 8) : s24_l;
        } else if (src_bits == 8) {
            s24_l = ((int)p[0] - 128) << 16;
            s24_r = (src_channels > 1) ? (((int)p[1] - 128) << 16) : s24_l;
        }

        short s16_l = dither_s24_to_s16(s24_l);
        short s16_r = dither_s24_to_s16(s24_r);

        unsigned int w_idx = g_queue_write_idx;
        g_pcm_queue[w_idx]     = s16_l;
        g_pcm_queue[w_idx + 1] = s16_r;
        g_queue_write_idx = (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
        g_queue_bytes += 4;
        produced_frames++;
    }

    return (int)produced_frames;
}

int audio_play_file(const char* filepath) {
    if (!filepath || !filepath[0]) return -1;

    enable_fpu();

    // 先停止当前播放
    audio_stop();

    if (!cmi8738_is_present()) {
        return -2; // 无输出设备
    }

    int len = my_strlen(filepath);
    char ext[5] = {0};
    int dot_pos = -1;
    for (int i = len - 1; i >= 0 && i >= len - 5; i--) {
        if (filepath[i] == '.') { dot_pos = i; break; }
    }
    if (dot_pos >= 0) {
        int k = 0;
        for (int i = dot_pos + 1; i < len && k < 4; i++) {
            char c = filepath[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            ext[k++] = c;
        }
        ext[k] = '\0';
    }

    int open_ok = -1;
    g_audio_info.title[0] = '\0';
    g_audio_info.current_time_s = 0;
    g_total_frames_played = 0;
    g_eof_reached = 0;

    // 1. WAV 解码尝试
    if (ext[0] == 'w' && ext[1] == 'a' && ext[2] == 'v') {
        wav_info_t wi;
        open_ok = wav_dec_open(filepath, &wi);
        if (open_ok == 0) {
            g_active_dec = DEC_WAV;
            g_audio_info.sample_rate = wi.sample_rate;
            g_audio_info.channels = wi.channels;
            g_audio_info.bit_depth = wi.bit_depth;
            g_audio_info.is_float = wi.is_float;
            g_audio_info.total_duration_s = wi.duration_s;
            g_audio_info.bitrate_kbps = (wi.sample_rate * wi.channels * wi.bit_depth) / 1000;
            g_audio_info.is_vbr = 0;
            my_strcpy(g_audio_info.title, filepath);
        }
    }
    // 2. FLAC 解码尝试
    else if (ext[0] == 'f' && ext[1] == 'l' && ext[2] == 'a' && ext[3] == 'c') {
        flac_info_t fi;
        open_ok = flac_dec_open(filepath, &fi);
        if (open_ok == 0) {
            g_active_dec = DEC_FLAC;
            g_audio_info.sample_rate = fi.sample_rate;
            g_audio_info.channels = 2; // flac_dec_read_samples 始终输出双声道
            g_audio_info.bit_depth = 16; // flac_dec_read_samples 始终输出 16-bit
            g_audio_info.is_float = 0;
            g_audio_info.total_duration_s = fi.duration_s;
            g_audio_info.bitrate_kbps = 0; // VBR
            g_audio_info.is_vbr = 1;
            if (fi.title[0]) my_strcpy(g_audio_info.title, fi.title);
            else my_strcpy(g_audio_info.title, filepath);
        }
    }
    // 3. MP3 解码尝试
    else if (ext[0] == 'm' && ext[1] == 'p' && ext[2] == '3') {
        mp3_info_t mi;
        open_ok = mp3_dec_open(filepath, &mi);
        if (open_ok == 0) {
            g_active_dec = DEC_MP3;
            g_audio_info.sample_rate = mi.sample_rate;
            g_audio_info.channels = 2; // mp3_dec_read_samples 始终输出双声道
            g_audio_info.bit_depth = 16; // mp3_dec_read_samples 始终输出 16-bit
            g_audio_info.is_float = 0;
            g_audio_info.total_duration_s = mi.duration_s;
            g_audio_info.bitrate_kbps = mi.bitrate_kbps;
            g_audio_info.is_vbr = mi.is_vbr;
            if (mi.title[0]) my_strcpy(g_audio_info.title, mi.title);
            else my_strcpy(g_audio_info.title, filepath);
        }
    }

    if (open_ok != 0) {
        g_active_dec = DEC_NONE;
        return -3; // 解码器打开失败
    }

    // 匹配声卡硬件目标采样率：44.1k 族或 48k 族
    int srate = g_audio_info.sample_rate;
    if (srate % 44100 == 0 || srate == 22050 || srate == 11025) {
        g_audio_info.target_rate = 44100;
        cmi8738_set_format(44100, 16, 1);
    } else {
        g_audio_info.target_rate = 48000;
        cmi8738_set_format(48000, 16, 1);
    }

    audio_flush_queue();
    g_total_frames_played = 0;
    g_eof_reached = 0;

    // 1. 静音 DMA 双半区，消除开机或切歌瞬间物理内存脏数据导致的 "si" 杂音
    cmi8738_silence_half(0);
    cmi8738_silence_half(1);

    // 2. 预充队列：在开启硬件 DMA 之前，先预充两半区 DMA + 内部队列缓冲（总计 ~96KB 缓冲）
    static unsigned char dec_temp_buf[16384];
    while (g_queue_bytes < (CMI_DMA_BUFFER_BYTES + CMI_DMA_HALF_BYTES) && !g_eof_reached) {
        int samples_read = 0;
        if (g_active_dec == DEC_WAV) samples_read = wav_dec_read_samples(dec_temp_buf, 2048);
        else if (g_active_dec == DEC_FLAC) samples_read = flac_dec_read_samples(dec_temp_buf, 4096);
        else if (g_active_dec == DEC_MP3) samples_read = mp3_dec_read_samples(dec_temp_buf, 1152);

        if (samples_read > 0) {
            audio_feed_raw_pcm(dec_temp_buf, (unsigned int)samples_read,
                               g_audio_info.sample_rate,
                               g_audio_info.channels,
                               g_audio_info.bit_depth,
                               g_audio_info.is_float);
        } else if (samples_read < 0) {
            g_eof_reached = 1;
            break;
        } else {
            break;
        }
    }

    // 3. 将预充好的 PCM 复制写入 DMA 半区 0 和半区 1
    static short dma_copy_buf[CMI_DMA_HALF_BYTES / sizeof(short)];
    for (int h = 0; h < 2; h++) {
        if (g_queue_bytes >= CMI_DMA_HALF_BYTES) {
            unsigned int samples_needed = CMI_DMA_HALF_BYTES / sizeof(short);
            for (unsigned int i = 0; i < samples_needed; i++) {
                dma_copy_buf[i] = g_pcm_queue[g_queue_read_idx];
                g_queue_read_idx = (g_queue_read_idx + 1) % (PCM_QUEUE_SIZE / sizeof(short));
            }
            g_queue_bytes -= CMI_DMA_HALF_BYTES;
            cmi8738_write_half(h, dma_copy_buf, CMI_DMA_HALF_BYTES);
        }
    }

    // 4. 启动声卡硬件 DMA 环形播放
    cmi8738_start_dma();
    g_audio_state = AUDIO_STATE_PLAYING;

    return 0;
}

void audio_pause(void) {
    if (g_audio_state == AUDIO_STATE_PLAYING) {
        cmi8738_stop_dma();
        g_audio_state = AUDIO_STATE_PAUSED;
    }
}

void audio_resume(void) {
    if (g_audio_state == AUDIO_STATE_PAUSED) {
        cmi8738_start_dma();
        g_audio_state = AUDIO_STATE_PLAYING;
    }
}

void audio_stop(void) {
    if (g_audio_state != AUDIO_STATE_STOPPED) {
        cmi8738_stop_dma();
        g_audio_state = AUDIO_STATE_STOPPED;
    }
    if (g_active_dec == DEC_WAV) wav_dec_close();
    else if (g_active_dec == DEC_FLAC) flac_dec_close();
    else if (g_active_dec == DEC_MP3) mp3_dec_close();

    g_active_dec = DEC_NONE;
    audio_flush_queue();
    g_total_frames_played = 0;
    g_eof_reached = 0;
    for (int i = 0; i < 10; i++) g_spectrum_bands[i] = 0;
    g_vu_l = 0; g_vu_r = 0;
}

void audio_seek_sec(unsigned int seconds) {
    if (g_active_dec == DEC_NONE) return;

    enable_fpu();

    if (g_active_dec == DEC_WAV) wav_dec_seek(seconds);
    else if (g_active_dec == DEC_FLAC) flac_dec_seek(seconds);
    else if (g_active_dec == DEC_MP3) mp3_dec_seek(seconds);

    audio_flush_queue();
    cmi8738_silence_half(0);
    cmi8738_silence_half(1);
    g_audio_info.current_time_s = (int)seconds;
    g_total_frames_played = (unsigned int)seconds * (unsigned int)g_audio_info.target_rate;
    g_eof_reached = 0;

    // 预充数据平滑恢复
    static unsigned char dec_temp_buf[16384];
    while (g_queue_bytes < CMI_DMA_HALF_BYTES && !g_eof_reached) {
        int samples_read = 0;
        if (g_active_dec == DEC_WAV) samples_read = wav_dec_read_samples(dec_temp_buf, 2048);
        else if (g_active_dec == DEC_FLAC) samples_read = flac_dec_read_samples(dec_temp_buf, 4096);
        else if (g_active_dec == DEC_MP3) samples_read = mp3_dec_read_samples(dec_temp_buf, 1152);

        if (samples_read > 0) {
            audio_feed_raw_pcm(dec_temp_buf, (unsigned int)samples_read,
                               g_audio_info.sample_rate,
                               g_audio_info.channels,
                               g_audio_info.bit_depth,
                               g_audio_info.is_float);
        } else if (samples_read < 0) {
            g_eof_reached = 1;
            break;
        } else {
            break;
        }
    }
}

audio_state_t audio_get_state(void) {
    return g_audio_state;
}

const char* audio_get_title(void) {
    return g_audio_info.title;
}

unsigned int audio_get_position_sec(void) {
    return (unsigned int)g_audio_info.current_time_s;
}

unsigned int audio_get_duration_sec(void) {
    return (unsigned int)g_audio_info.total_duration_s;
}

void audio_set_time(int current_s) {
    if (current_s >= 0) {
        g_audio_info.current_time_s = current_s;
    }
}

void audio_set_bitrate(int kbps, int is_vbr) {
    g_audio_info.bitrate_kbps = kbps;
    g_audio_info.is_vbr = is_vbr;
}

void audio_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_master_volume = vol;
    if (cmi8738_is_present()) {
        int v = g_is_muted ? 0 : g_master_volume;
        cmi8738_set_volume(v, v);
    }
}

int audio_get_volume(void) {
    return g_is_muted ? 0 : g_master_volume;
}

void audio_toggle_mute(void) {
    g_is_muted = !g_is_muted;
    if (cmi8738_is_present()) {
        int v = g_is_muted ? 0 : g_master_volume;
        cmi8738_set_volume(v, v);
    }
}

int audio_is_muted(void) {
    return g_is_muted;
}

const audio_info_t* audio_get_info(void) {
    return &g_audio_info;
}

// 主循环泵送音频数据与频谱计算
void audio_poll(void) {
    if (g_audio_state != AUDIO_STATE_PLAYING) return;
    if (!cmi8738_is_present()) return;

    // 1. 如果内部队列有空间，从当前解码器读取新样本
    // 贪婪循环填充至高水位（48KB），单次最多解码 6 帧以防阻塞 GUI
    int dec_count = 0;
    static unsigned char dec_temp_buf[16384];
    while (!g_eof_reached && g_queue_bytes < (PCM_QUEUE_SIZE - 16384) && dec_count < 6) {
        dec_count++;
        int samples_read = 0;

        if (g_active_dec == DEC_WAV) {
            samples_read = wav_dec_read_samples(dec_temp_buf, 2048);
        } else if (g_active_dec == DEC_FLAC) {
            samples_read = flac_dec_read_samples(dec_temp_buf, 4096);
        } else if (g_active_dec == DEC_MP3) {
            samples_read = mp3_dec_read_samples(dec_temp_buf, 1152);
        }

        if (samples_read > 0) {
            audio_feed_raw_pcm(dec_temp_buf, (unsigned int)samples_read,
                               g_audio_info.sample_rate,
                               g_audio_info.channels,
                               g_audio_info.bit_depth,
                               g_audio_info.is_float);
        } else if (samples_read < 0) {
            g_eof_reached = 1;
            break;
        } else {
            // samples_read == 0: 暂无新样本，跳出
            break;
        }
    }

    // 2. 检查声卡 DMA 半区是否已播放完需要填充
    int half_needed = cmi8738_poll_half_needed();
    if (half_needed >= 0) {
        static short dma_copy_buf[CMI_DMA_HALF_BYTES / sizeof(short)];
        if (g_queue_bytes >= CMI_DMA_HALF_BYTES) {
            unsigned int samples_needed = CMI_DMA_HALF_BYTES / sizeof(short);
            for (unsigned int i = 0; i < samples_needed; i++) {
                dma_copy_buf[i] = g_pcm_queue[g_queue_read_idx];
                g_queue_read_idx = (g_queue_read_idx + 1) % (PCM_QUEUE_SIZE / sizeof(short));
            }
            g_queue_bytes -= CMI_DMA_HALF_BYTES;
            cmi8738_write_half(half_needed, dma_copy_buf, CMI_DMA_HALF_BYTES);
            g_total_frames_played += (CMI_DMA_HALF_BYTES / 4);
        } else if (g_queue_bytes > 0) {
            // 欠载零填充保护
            unsigned int avail_samples = g_queue_bytes / sizeof(short);
            for (unsigned int i = 0; i < avail_samples; i++) {
                dma_copy_buf[i] = g_pcm_queue[g_queue_read_idx];
                g_queue_read_idx = (g_queue_read_idx + 1) % (PCM_QUEUE_SIZE / sizeof(short));
            }
            for (unsigned int i = avail_samples; i < CMI_DMA_HALF_BYTES / sizeof(short); i++) {
                dma_copy_buf[i] = 0;
            }
            g_queue_bytes = 0;
            cmi8738_write_half(half_needed, dma_copy_buf, CMI_DMA_HALF_BYTES);
            g_total_frames_played += (avail_samples / 2);
        } else {
            // 完全排空静音
            cmi8738_silence_half(half_needed);
            if (g_eof_reached) {
                audio_stop();
                return;
            }
        }

        // 更新当前播放秒数
        int tr = g_audio_info.target_rate;
        if (tr > 0) {
            g_audio_info.current_time_s = (int)(g_total_frames_played / (unsigned int)tr);
        }
    }

    // 3. 硬件播放位置同步的 10 段频谱仪与立体声 VU 表
    unsigned int frame = cmi8738_get_dma_frame_pos();
    unsigned int sample_offset = (frame * 2) % (CMI_DMA_BUFFER_BYTES / sizeof(short));
    const short* dma_samples = (const short*)cmi8738_get_dma_buffer();
    if (dma_samples) {
        int peak_l = 0, peak_r = 0;
        int band_acc[10] = {0};

        for (int i = 0; i < 128; i++) {
            unsigned int idx = (sample_offset + i * 2) % (CMI_DMA_BUFFER_BYTES / sizeof(short));
            int sl = dma_samples[idx];
            int sr = dma_samples[idx + 1];
            if (sl < 0) sl = -sl;
            if (sr < 0) sr = -sr;
            if (sl > peak_l) peak_l = sl;
            if (sr > peak_r) peak_r = sr;

            int diff = sl - sr;
            if (diff < 0) diff = -diff;
            int sum = (sl + sr) >> 1;
            band_acc[i % 10] += (i < 40) ? (sum >> 3) : (diff >> 2);
        }

        int cur_l = (peak_l * 255) / 32768;
        int cur_r = (peak_r * 255) / 32768;
        if (cur_l > g_vu_l) g_vu_l = (unsigned char)cur_l;
        else g_vu_l = (unsigned char)((g_vu_l * 8) / 10);
        if (cur_r > g_vu_r) g_vu_r = (unsigned char)cur_r;
        else g_vu_r = (unsigned char)((g_vu_r * 8) / 10);

        for (int b = 0; b < 10; b++) {
            int target = (band_acc[b] * 16) / 128;
            if (target > 15) target = 15;
            if (target > (int)g_spectrum_bands[b]) g_spectrum_bands[b] = (unsigned char)target;
            else if (g_spectrum_bands[b] > 0) g_spectrum_bands[b]--;
        }
    }
}

void audio_get_spectrum(unsigned char bands[10]) {
    for (int i = 0; i < 10; i++) {
        bands[i] = g_spectrum_bands[i];
    }
}

void audio_get_vu(unsigned char* left_vu, unsigned char* right_vu) {
    if (left_vu)  *left_vu = g_vu_l;
    if (right_vu) *right_vu = g_vu_r;
}
