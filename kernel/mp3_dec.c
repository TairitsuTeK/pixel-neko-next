#include "mp3_dec.h"
#include "vfs.h"
#include "audio.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

static mp3dec_t g_mp3d;

// MP3 (MPEG-1/2 Audio Layer III) Decoder

static vfs_stream_t g_mp3_stream;
static mp3_info_t g_mp3_info;
static int g_mp3_is_open = 0;
static unsigned int g_first_frame_pos = 0;
static unsigned int g_cur_frame_idx = 0;
static unsigned char g_vbr_toc[100];
static int g_has_vbr_toc = 0;

// Standard MPEG Layer III Bitrate Tables (kbps)
static const short g_bitrate_tab[2][16] = {
    // MPEG 1 Layer III
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
    // MPEG 2 / 2.5 Layer III
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }
};

// Standard MPEG Sample Rate Tables (Hz)
static const int g_samplerate_tab[4][4] = {
    { 11025, 12000, 8000, 0 },  // MPEG 2.5
    { 0, 0, 0, 0 },             // Reserved
    { 22050, 24000, 16000, 0 },  // MPEG 2
    { 44100, 48000, 32000, 0 }   // MPEG 1
};

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static unsigned int parse_syncsafe(const unsigned char* p) {
    return ((unsigned int)(p[0] & 0x7F) << 21) |
           ((unsigned int)(p[1] & 0x7F) << 14) |
           ((unsigned int)(p[2] & 0x7F) << 7)  |
           ((unsigned int)(p[3] & 0x7F));
}

// Parse ID3v2 header and tags
static unsigned int parse_id3v2(void) {
    unsigned char hdr[10];
    vfs_stream_seek(&g_mp3_stream, 0);
    if (vfs_stream_read(&g_mp3_stream, hdr, 10) < 10) return 0;

    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return 0;

    unsigned int tag_size = parse_syncsafe(hdr + 6) + 10;
    if (tag_size > g_mp3_stream.size) tag_size = 0;

    // Scan ID3v2 frames (TIT2, TPE1, TALB)
    unsigned int pos = 10;
    unsigned char frame_hdr[10];
    while (pos + 10 < tag_size) {
        vfs_stream_seek(&g_mp3_stream, pos);
        if (vfs_stream_read(&g_mp3_stream, frame_hdr, 10) < 10) break;
        if (frame_hdr[0] == 0) break; // Padding reached

        unsigned int fsize = ((unsigned int)frame_hdr[4] << 24) | ((unsigned int)frame_hdr[5] << 16) |
                             ((unsigned int)frame_hdr[6] << 8)  | (unsigned int)frame_hdr[7];
        // Handle syncsafe size if ID3v2.4
        if (hdr[3] >= 4) fsize = parse_syncsafe(frame_hdr + 4);
        if (fsize == 0 || pos + 10 + fsize > tag_size) break;

        char id[5] = { (char)frame_hdr[0], (char)frame_hdr[1], (char)frame_hdr[2], (char)frame_hdr[3], 0 };
        if (id[0] == 'T' && id[1] == 'I' && id[2] == 'T' && id[3] == '2') {
            unsigned char tbuf[64];
            vfs_stream_seek(&g_mp3_stream, pos + 11); // Skip 1-byte encoding
            int r = vfs_stream_read(&g_mp3_stream, tbuf, (fsize - 1) < 63 ? (fsize - 1) : 63);
            if (r > 0) { tbuf[r] = 0; str_copy(g_mp3_info.title, (char*)tbuf, 64); }
        } else if (id[0] == 'T' && id[1] == 'P' && id[2] == 'E' && id[3] == '1') {
            unsigned char tbuf[64];
            vfs_stream_seek(&g_mp3_stream, pos + 11);
            int r = vfs_stream_read(&g_mp3_stream, tbuf, (fsize - 1) < 63 ? (fsize - 1) : 63);
            if (r > 0) { tbuf[r] = 0; str_copy(g_mp3_info.artist, (char*)tbuf, 64); }
        } else if (id[0] == 'T' && id[1] == 'A' && id[2] == 'L' && id[3] == 'B') {
            unsigned char tbuf[64];
            vfs_stream_seek(&g_mp3_stream, pos + 11);
            int r = vfs_stream_read(&g_mp3_stream, tbuf, (fsize - 1) < 63 ? (fsize - 1) : 63);
            if (r > 0) { tbuf[r] = 0; str_copy(g_mp3_info.album, (char*)tbuf, 64); }
        }

        pos += 10 + fsize;
    }

    return tag_size;
}

// Parse ID3v1 tag at end of file
static void parse_id3v1(void) {
    if (g_mp3_stream.size < 128) return;
    vfs_stream_seek(&g_mp3_stream, g_mp3_stream.size - 128);
    unsigned char v1[128];
    if (vfs_stream_read(&g_mp3_stream, v1, 128) == 128) {
        if (v1[0] == 'T' && v1[1] == 'A' && v1[2] == 'G') {
            if (g_mp3_info.title[0] == 0) {
                char t[31]; for(int i=0;i<30;i++) t[i]=v1[3+i]; t[30]=0;
                str_copy(g_mp3_info.title, t, 31);
            }
            if (g_mp3_info.artist[0] == 0) {
                char t[31]; for(int i=0;i<30;i++) t[i]=v1[33+i]; t[30]=0;
                str_copy(g_mp3_info.artist, t, 31);
            }
            if (g_mp3_info.album[0] == 0) {
                char t[31]; for(int i=0;i<30;i++) t[i]=v1[63+i]; t[30]=0;
                str_copy(g_mp3_info.album, t, 31);
            }
        }
    }
}

// Check and parse MP3 frame header at given stream offset
// Returns frame length in bytes, or 0 if not a valid header
static int check_frame_header(unsigned int offset, int* out_rate, int* out_ch, int* out_kbps, int* out_samples) {
    unsigned char hdr[4];
    vfs_stream_seek(&g_mp3_stream, offset);
    if (vfs_stream_read(&g_mp3_stream, hdr, 4) < 4) return 0;

    // Sync word: 11 bits of 1 (0xFFE0 mask)
    if (hdr[0] != 0xFF || (hdr[1] & 0xE0) != 0xE0) return 0;

    int ver_idx = (hdr[1] >> 3) & 0x03; // 0=2.5, 2=2.0, 3=1.0
    int layer_idx = (hdr[1] >> 1) & 0x03; // 1=Layer III
    if (ver_idx == 1 || layer_idx != 1) return 0; // Layer III only

    int br_idx = (hdr[2] >> 4) & 0x0F;
    int sr_idx = (hdr[2] >> 2) & 0x03;
    int padding = (hdr[2] >> 1) & 0x01;
    int channel_mode = (hdr[3] >> 6) & 0x03;

    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return 0;

    int is_mpeg1 = (ver_idx == 3);
    int kbps = g_bitrate_tab[is_mpeg1 ? 0 : 1][br_idx];
    int rate = g_samplerate_tab[ver_idx][sr_idx];
    if (rate <= 0 || kbps <= 0) return 0;

    int channels = (channel_mode == 3) ? 1 : 2;
    int samples_per_frame = is_mpeg1 ? 1152 : 576;
    int frame_len = 0;

    if (is_mpeg1) {
        frame_len = (144 * kbps * 1000) / rate + padding;
    } else {
        frame_len = (72 * kbps * 1000) / rate + padding;
    }

    if (out_rate) *out_rate = rate;
    if (out_ch) *out_ch = channels;
    if (out_kbps) *out_kbps = kbps;
    if (out_samples) *out_samples = samples_per_frame;

    return frame_len;
}

// Find first audio sync frame and parse Xing / VBRI VBR headers
static int find_first_frame(unsigned int search_start) {
    unsigned int pos = search_start;
    unsigned int max_search = search_start + 65536;
    if (max_search > g_mp3_stream.size) max_search = g_mp3_stream.size;

    while (pos + 4 < max_search) {
        int rate = 0, ch = 0, kbps = 0, samples = 0;
        int flen = check_frame_header(pos, &rate, &ch, &kbps, &samples);
        if (flen > 0) {
            // Found valid frame header
            g_first_frame_pos = pos;
            g_mp3_info.sample_rate = rate;
            g_mp3_info.channels = ch;
            g_mp3_info.bitrate_kbps = kbps;

            // Check for Xing / Info header in this first frame
            unsigned char fbuf[128];
            vfs_stream_seek(&g_mp3_stream, pos);
            int r = vfs_stream_read(&g_mp3_stream, fbuf, 128);
            if (r >= 64) {
                // Check Xing offset (36 for MPEG1 stereo, 21 for others)
                int xing_off = (ch == 2 && rate >= 32000) ? 36 : 21;
                if (pos + xing_off + 8 < g_mp3_stream.size) {
                    const unsigned char* xp = fbuf + xing_off;
                    if ((xp[0] == 'X' && xp[1] == 'i' && xp[2] == 'n' && xp[3] == 'g') ||
                        (xp[0] == 'I' && xp[1] == 'n' && xp[2] == 'f' && xp[3] == 'o')) {
                        g_mp3_info.is_vbr = (xp[0] == 'X');
                        unsigned int flags = ((unsigned int)xp[4] << 24) | ((unsigned int)xp[5] << 16) |
                                             ((unsigned int)xp[6] << 8)  | (unsigned int)xp[7];
                        int cur = 8;
                        if (flags & 1) { // Frames field
                            g_mp3_info.total_frames = ((unsigned int)xp[cur] << 24) | ((unsigned int)xp[cur+1] << 16) |
                                                      ((unsigned int)xp[cur+2] << 8) | (unsigned int)xp[cur+3];
                            cur += 4;
                        }
                        if (flags & 2) { // Bytes field
                            cur += 4;
                        }
                        if (flags & 4) { // TOC field (100 bytes)
                            for (int t = 0; t < 100; t++) g_vbr_toc[t] = xp[cur + t];
                            g_has_vbr_toc = 1;
                        }
                    }
                }
            }

            // If no Xing frames, calculate from file size
            if (g_mp3_info.total_frames == 0 && kbps > 0) {
                unsigned int audio_bytes = g_mp3_stream.size - g_first_frame_pos;
                g_mp3_info.duration_s = (int)((audio_bytes * 8) / (kbps * 1000));
                g_mp3_info.total_frames = (g_mp3_info.duration_s * rate) / samples;
            } else if (g_mp3_info.total_frames > 0 && rate > 0) {
                g_mp3_info.duration_s = (int)(((unsigned int)g_mp3_info.total_frames * (unsigned int)samples) / (unsigned int)rate);
            }

            if (g_mp3_info.duration_s <= 0) {
                g_mp3_info.duration_s = 1; // Default fallback to avoid --:--
            }

            return 0;
        }
        pos++;
    }

    return -1;
}

int mp3_dec_open(const char* path, mp3_info_t* out_info) {
    mp3_dec_close();

    if (vfs_stream_open(path, &g_mp3_stream) != 0) {
        return -1;
    }

    g_mp3_info.title[0] = '\0';
    g_mp3_info.artist[0] = '\0';
    g_mp3_info.album[0] = '\0';
    g_mp3_info.sample_rate = 44100;
    g_mp3_info.channels = 2;
    g_mp3_info.bitrate_kbps = 128;
    g_mp3_info.is_vbr = 0;
    g_mp3_info.duration_s = 0;
    g_mp3_info.total_frames = 0;
    g_has_vbr_toc = 0;

    // Parse ID3v2 & ID3v1 metadata
    unsigned int id3_size = parse_id3v2();
    parse_id3v1();

    // Scan for first audio frame
    if (find_first_frame(id3_size) != 0) {
        vfs_stream_close(&g_mp3_stream);
        return -1;
    }

    // Default title fallback
    if (g_mp3_info.title[0] == '\0') {
        const char* fn = path;
        for (int k = 0; path[k]; k++) {
            if (path[k] == '\\' || path[k] == '/') fn = &path[k + 1];
        }
        str_copy(g_mp3_info.title, fn, sizeof(g_mp3_info.title));
    }

    g_cur_frame_idx = 0;
    vfs_stream_seek(&g_mp3_stream, g_first_frame_pos);
    mp3dec_init(&g_mp3d);
    s_mp3_buf_len = 0;
    g_mp3_is_open = 1;

    if (out_info) {
        *out_info = g_mp3_info;
    }

    audio_set_bitrate(g_mp3_info.bitrate_kbps, g_mp3_info.is_vbr);

    return 0;
}

static unsigned char s_mp3_buf[8192];
static int s_mp3_buf_len = 0;

int mp3_dec_read_samples(void* out_samples, unsigned int max_samples) {
    if (!g_mp3_is_open) return -1;

    // 顺序预充流式滑动窗口，完全规避反向 seek 和频繁零散读盘
    while (s_mp3_buf_len < 4096 && g_mp3_stream.current_offset < g_mp3_stream.size) {
        int space = (int)sizeof(s_mp3_buf) - s_mp3_buf_len;
        if (space <= 0) break;
        int nread = vfs_stream_read(&g_mp3_stream, s_mp3_buf + s_mp3_buf_len, space);
        if (nread <= 0) break;
        s_mp3_buf_len += nread;
    }

    if (s_mp3_buf_len <= 0) {
        return -1; // EOF reached and buffer drained
    }

    mp3dec_frame_info_t info;
    info.frame_bytes = 0;
    static short pcm_frame[MINIMP3_MAX_SAMPLES_PER_FRAME]; // 1152 * 2
    int samples = mp3dec_decode_frame(&g_mp3d, s_mp3_buf, s_mp3_buf_len, pcm_frame, &info);

    int consumed = info.frame_bytes;
    if (consumed > s_mp3_buf_len) consumed = s_mp3_buf_len;

    if (consumed > 0) {
        mp3_memmove(s_mp3_buf, s_mp3_buf + consumed, s_mp3_buf_len - consumed);
        s_mp3_buf_len -= consumed;
    } else if (samples == 0) {
        // 未解析出有效帧且未消耗字节（跳过脏数据）：向后滑动 1 字节
        if (s_mp3_buf_len > 0) {
            mp3_memmove(s_mp3_buf, s_mp3_buf + 1, s_mp3_buf_len - 1);
            s_mp3_buf_len--;
        }
    }

    if (samples > 0) {
        g_cur_frame_idx++;
        if (info.bitrate_kbps > 0) {
            audio_set_bitrate(info.bitrate_kbps, g_mp3_info.is_vbr);
        }
        if (info.hz > 0) {
            unsigned int cur_time_s = (g_cur_frame_idx * (unsigned int)samples) / (unsigned int)info.hz;
            audio_set_time((int)cur_time_s);
        }

        unsigned int frames_to_copy = (unsigned int)samples;
        if (frames_to_copy > max_samples) frames_to_copy = max_samples;
        short* dst = (short*)out_samples;
        int ch = info.channels;

        for (unsigned int i = 0; i < frames_to_copy; i++) {
            if (ch == 1) {
                dst[i * 2]     = pcm_frame[i];
                dst[i * 2 + 1] = pcm_frame[i];
            } else {
                dst[i * 2]     = pcm_frame[i * 2];
                dst[i * 2 + 1] = pcm_frame[i * 2 + 1];
            }
        }

        return (int)frames_to_copy;
    }

    if (g_mp3_stream.current_offset >= g_mp3_stream.size && s_mp3_buf_len < 4) {
        return -1; // 流到末尾且不足以解码一帧
    }

    return 0; // In sync scan, not EOF
}

int mp3_dec_seek(unsigned int seconds) {
    if (!g_mp3_is_open) return -1;
    if (g_mp3_info.duration_s <= 0) return -1;

    unsigned int target_offset = g_first_frame_pos;

    if (g_has_vbr_toc && g_mp3_info.duration_s > 0) {
        int percent = (seconds * 100) / g_mp3_info.duration_s;
        if (percent < 0) percent = 0;
        if (percent > 99) percent = 99;
        unsigned int audio_size = g_mp3_stream.size - g_first_frame_pos;
        unsigned int rel_bytes = ((unsigned int)g_vbr_toc[percent] * audio_size) / 256;
        target_offset = g_first_frame_pos + rel_bytes;
    } else if (g_mp3_info.duration_s > 0) {
        unsigned int audio_size = g_mp3_stream.size - g_first_frame_pos;
        target_offset = g_first_frame_pos + (seconds * audio_size) / g_mp3_info.duration_s;
    }

    if (target_offset >= g_mp3_stream.size) target_offset = g_mp3_stream.size - 2048;
    vfs_stream_seek(&g_mp3_stream, target_offset);
    s_mp3_buf_len = 0;
    mp3dec_init(&g_mp3d);

    g_cur_frame_idx = (seconds * (unsigned int)g_mp3_info.sample_rate) / 1152;
    audio_set_time((int)seconds);

    return 0;
}

void mp3_dec_close(void) {
    if (g_mp3_is_open) {
        vfs_stream_close(&g_mp3_stream);
        s_mp3_buf_len = 0;
        g_mp3_is_open = 0;
    }
}
