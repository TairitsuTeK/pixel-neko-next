#include "flac_dec.h"
#include "vfs.h"
#include "audio.h"

// FLAC (Free Lossless Audio Codec) Pure C Integer Decoder

static vfs_stream_t g_flac_stream;
static flac_info_t g_flac_info;
static int g_flac_is_open = 0;
static unsigned int g_first_frame_offset = 0;
static unsigned int g_cur_sample_idx = 0;

// Subframe buffers (supports up to 4096 samples/block, 2 channels)
#define FLAC_MAX_BLOCK_SIZE 4096
static int g_subframe_buf[2][FLAC_MAX_BLOCK_SIZE];

// Bitstream reader state
typedef struct {
    unsigned int tag;
    int bitcount;
} flac_bitreader_t;

static flac_bitreader_t g_br;

// 64-bit arithmetic shift right using x86 shrdl instruction
// Completely eliminates libgcc __ashrdi3 call in freestanding kernel
static inline int sar64_to_32(long long val, unsigned char shift) {
    unsigned int lo = (unsigned int)val;
    int hi = (int)(val >> 32);
    if (shift == 0) return (int)lo;
    if (shift < 32) {
        __asm__ volatile ("shrdl %2, %1, %0" : "+r"(lo) : "r"(hi), "c"(shift));
        return (int)lo;
    } else {
        return hi >> (shift - 32);
    }
}

static inline void br_reset(void) {
    g_br.tag = 0;
    g_br.bitcount = 0;
}

static inline unsigned int br_read_bits(int num) {
    while (g_br.bitcount < num) {
        unsigned char b = 0;
        vfs_stream_read(&g_flac_stream, &b, 1);
        g_br.tag |= ((unsigned int)b << (24 - g_br.bitcount));
        g_br.bitcount += 8;
    }
    unsigned int val = g_br.tag >> (32 - num);
    g_br.tag <<= num;
    g_br.bitcount -= num;
    return val;
}

static inline int br_read_signed_bits(int num) {
    unsigned int u = br_read_bits(num);
    int sign_shift = 32 - num;
    return (int)(u << sign_shift) >> sign_shift;
}

static inline unsigned int br_read_unary(void) {
    unsigned int count = 0;
    while (br_read_bits(1) == 0) {
        count++;
        if (count > 65536) break; // Safety limit
    }
    return count;
}

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int flac_dec_open(const char* path, flac_info_t* out_info) {
    flac_dec_close();

    if (vfs_stream_open(path, &g_flac_stream) != 0) {
        return -1;
    }

    // Verify 'fLaC' marker
    unsigned char marker[4];
    if (vfs_stream_read(&g_flac_stream, marker, 4) < 4) {
        vfs_stream_close(&g_flac_stream);
        return -1;
    }
    if (marker[0] != 'f' || marker[1] != 'L' || marker[2] != 'a' || marker[3] != 'C') {
        vfs_stream_close(&g_flac_stream);
        return -1; // Not a FLAC file
    }

    g_flac_info.title[0] = '\0';
    g_flac_info.artist[0] = '\0';
    g_flac_info.album[0] = '\0';
    g_flac_info.sample_rate = 44100;
    g_flac_info.channels = 2;
    g_flac_info.bit_depth = 16;
    g_flac_info.total_samples = 0;
    g_flac_info.duration_s = 0;

    int is_last = 0;
    int found_streaminfo = 0;

    // Scan metadata blocks
    while (!is_last) {
        unsigned char header[4];
        if (vfs_stream_read(&g_flac_stream, header, 4) < 4) break;

        is_last = (header[0] & 0x80) ? 1 : 0;
        int block_type = header[0] & 0x7F;
        unsigned int block_len = ((unsigned int)header[1] << 16) | ((unsigned int)header[2] << 8) | (unsigned int)header[3];

        if (block_type == 0 && block_len >= 34) {
            // METADATA_BLOCK_STREAMINFO
            unsigned char sinfo[34];
            if (vfs_stream_read(&g_flac_stream, sinfo, 34) == 34) {
                // sample_rate: 20 bits (sinfo[10..12])
                g_flac_info.sample_rate = ((int)sinfo[10] << 12) | ((int)sinfo[11] << 4) | ((int)(sinfo[12] >> 4));
                // channels: 3 bits (sinfo[12] bits 3:1) + 1
                g_flac_info.channels = ((sinfo[12] >> 1) & 0x07) + 1;
                // bit_depth: 5 bits (sinfo[12] bit 0 and sinfo[13] bits 7:4) + 1
                g_flac_info.bit_depth = (((sinfo[12] & 1) << 4) | (sinfo[13] >> 4)) + 1;
                // total_samples: 36 bits (lower 4 bits of sinfo[13] and sinfo[14..17])
                unsigned int hi_samp = sinfo[13] & 0x0F;
                unsigned int lo_samp = ((unsigned int)sinfo[14] << 24) | ((unsigned int)sinfo[15] << 16) |
                                       ((unsigned int)sinfo[16] << 8) | (unsigned int)sinfo[17];
                g_flac_info.total_samples = (hi_samp ? 0xFFFFFFFF : lo_samp);
                if (g_flac_info.sample_rate > 0) {
                    g_flac_info.duration_s = g_flac_info.total_samples / g_flac_info.sample_rate;
                }
                found_streaminfo = 1;
            }
            if (block_len > 34) {
                vfs_stream_seek(&g_flac_stream, g_flac_stream.current_offset + (block_len - 34));
            }
        } else if (block_type == 4) {
            // VORBIS_COMMENT: Extract basic metadata
            unsigned char cbuf[512];
            unsigned int rlen = block_len < 512 ? block_len : 512;
            if (vfs_stream_read(&g_flac_stream, cbuf, rlen) > 0) {
                // Quick scan for "TITLE=", "ARTIST=", "ALBUM="
                for (unsigned int k = 0; k + 8 < rlen; k++) {
                    if (cbuf[k] == 'T' && cbuf[k+1] == 'I' && cbuf[k+2] == 'T' && cbuf[k+3] == 'L' && cbuf[k+4] == 'E' && cbuf[k+5] == '=') {
                        int p = 0; unsigned int idx = k + 6;
                        while (idx < rlen && cbuf[idx] >= 32 && p < 63) { g_flac_info.title[p++] = cbuf[idx++]; }
                        g_flac_info.title[p] = '\0';
                    }
                    if (cbuf[k] == 'A' && cbuf[k+1] == 'R' && cbuf[k+2] == 'T' && cbuf[k+3] == 'I' && cbuf[k+4] == 'S' && cbuf[k+5] == 'T' && cbuf[k+6] == '=') {
                        int p = 0; unsigned int idx = k + 7;
                        while (idx < rlen && cbuf[idx] >= 32 && p < 63) { g_flac_info.artist[p++] = cbuf[idx++]; }
                        g_flac_info.artist[p] = '\0';
                    }
                    if (cbuf[k] == 'A' && cbuf[k+1] == 'L' && cbuf[k+2] == 'B' && cbuf[k+3] == 'U' && cbuf[k+4] == 'M' && cbuf[k+5] == '=') {
                        int p = 0; unsigned int idx = k + 6;
                        while (idx < rlen && cbuf[idx] >= 32 && p < 63) { g_flac_info.album[p++] = cbuf[idx++]; }
                        g_flac_info.album[p] = '\0';
                    }
                }
            }
            if (block_len > rlen) {
                vfs_stream_seek(&g_flac_stream, g_flac_stream.current_offset + (block_len - rlen));
            }
        } else {
            // Skip other metadata blocks
            vfs_stream_seek(&g_flac_stream, g_flac_stream.current_offset + block_len);
        }
    }

    if (!found_streaminfo || g_flac_info.channels <= 0 || g_flac_info.sample_rate <= 0) {
        vfs_stream_close(&g_flac_stream);
        return -1;
    }

    g_first_frame_offset = g_flac_stream.current_offset;
    g_cur_sample_idx = 0;
    br_reset();
    g_flac_is_open = 1;

    if (out_info) {
        *out_info = g_flac_info;
    }

    // Default title fallback if tags absent
    if (g_flac_info.title[0] == '\0') {
        const char* fn = path;
        for (int k = 0; path[k]; k++) {
            if (path[k] == '\\' || path[k] == '/') fn = &path[k + 1];
        }
        str_copy(g_flac_info.title, fn, sizeof(g_flac_info.title));
    }

    int kbps = (int)(((unsigned int)g_flac_info.sample_rate * g_flac_info.bit_depth * g_flac_info.channels) / 2000); // Typical FLAC compression ~50%
    audio_set_bitrate(kbps, 1);

    return 0;
}

// Subframe decoder
static int decode_subframe(int ch, int block_size, int bps) {
    if (br_read_bits(1) != 0) return -1; // Zero bit required
    int type = br_read_bits(6);
    int wasted_bits = br_read_bits(1);
    if (wasted_bits) {
        wasted_bits = br_read_unary() + 1;
        bps -= wasted_bits;
    }

    int* samples = g_subframe_buf[ch];

    if (type == 0) {
        // CONSTANT
        int val = br_read_signed_bits(bps);
        for (int i = 0; i < block_size; i++) samples[i] = val;
    } else if (type == 1) {
        // VERBATIM
        for (int i = 0; i < block_size; i++) samples[i] = br_read_signed_bits(bps);
    } else if (type >= 8 && type <= 12) {
        // FIXED PREDICTOR (order 0..4)
        int order = type - 8;
        for (int i = 0; i < order; i++) samples[i] = br_read_signed_bits(bps);

        // Decode Rice residuals
        int coding_method = br_read_bits(2);
        int partition_order = br_read_bits(4);
        int partitions = 1 << partition_order;
        int param_bits = (coding_method == 0) ? 4 : 5;
        int escape_param = (coding_method == 0) ? 15 : 31;

        int sample_idx = order;
        for (int p = 0; p < partitions; p++) {
            int p_samples = (p == 0) ? ((block_size >> partition_order) - order) : (block_size >> partition_order);
            int rice_param = br_read_bits(param_bits);
            if (rice_param == escape_param) {
                int raw_bits = br_read_bits(5);
                for (int s = 0; s < p_samples; s++) samples[sample_idx++] = br_read_signed_bits(raw_bits);
            } else {
                for (int s = 0; s < p_samples; s++) {
                    unsigned int msb = br_read_unary();
                    unsigned int lsb = br_read_bits(rice_param);
                    unsigned int u = (msb << rice_param) | lsb;
                    int res = (u & 1) ? -(int)(u >> 1) - 1 : (int)(u >> 1);
                    samples[sample_idx++] = res;
                }
            }
        }

        // Apply fixed predictor restoration
        for (int i = order; i < block_size; i++) {
            int pred = 0;
            if (order == 1) pred = samples[i - 1];
            else if (order == 2) pred = 2 * samples[i - 1] - samples[i - 2];
            else if (order == 3) pred = 3 * samples[i - 1] - 3 * samples[i - 2] + samples[i - 3];
            else if (order == 4) pred = 4 * samples[i - 1] - 6 * samples[i - 2] + 4 * samples[i - 3] - samples[i - 4];
            samples[i] += pred;
        }
    } else if (type >= 32) {
        // LPC PREDICTOR (order 1..32)
        int order = type - 31;
        for (int i = 0; i < order; i++) samples[i] = br_read_signed_bits(bps);

        int qlp_precision = br_read_bits(4) + 1;
        int qlp_shift = br_read_signed_bits(5);
        short qlp_coeffs[32];
        for (int i = 0; i < order; i++) qlp_coeffs[i] = (short)br_read_signed_bits(qlp_precision);

        // Decode Rice residuals
        int coding_method = br_read_bits(2);
        int partition_order = br_read_bits(4);
        int partitions = 1 << partition_order;
        int param_bits = (coding_method == 0) ? 4 : 5;
        int escape_param = (coding_method == 0) ? 15 : 31;

        int sample_idx = order;
        for (int p = 0; p < partitions; p++) {
            int p_samples = (p == 0) ? ((block_size >> partition_order) - order) : (block_size >> partition_order);
            int rice_param = br_read_bits(param_bits);
            if (rice_param == escape_param) {
                int raw_bits = br_read_bits(5);
                for (int s = 0; s < p_samples; s++) samples[sample_idx++] = br_read_signed_bits(raw_bits);
            } else {
                for (int s = 0; s < p_samples; s++) {
                    unsigned int msb = br_read_unary();
                    unsigned int lsb = br_read_bits(rice_param);
                    unsigned int u = (msb << rice_param) | lsb;
                    int res = (u & 1) ? -(int)(u >> 1) - 1 : (int)(u >> 1);
                    samples[sample_idx++] = res;
                }
            }
        }

        // Apply LPC FIR filter using sar64_to_32 inline assembly (no __ashrdi3 libgcc dependency)
        for (int i = order; i < block_size; i++) {
            long long sum = 0;
            for (int j = 0; j < order; j++) {
                sum += (long long)qlp_coeffs[j] * (long long)samples[i - 1 - j];
            }
            int pred = sar64_to_32(sum, (unsigned char)(qlp_shift > 0 ? qlp_shift : 0));
            samples[i] += pred;
        }
    }

    if (wasted_bits > 0) {
        for (int i = 0; i < block_size; i++) samples[i] <<= wasted_bits;
    }

    return 0;
}

int flac_dec_read_samples(void* out_samples, unsigned int max_samples) {
    if (!g_flac_is_open) return 0;
    (void)out_samples; // Samples are fed directly to audio_feed_raw_pcm
    (void)max_samples;

    // Sync to next frame header (14-bit sync code 0x3FFE)
    int sync_found = 0;
    while (!sync_found) {
        unsigned int w = br_read_bits(8);
        if (w == 0xFF) {
            unsigned int next = br_read_bits(6);
            if (next == 0x3E) {
                sync_found = 1;
                break;
            }
        }
        if (g_flac_stream.current_offset >= g_flac_stream.size) return -1; // EOF
    }

    // Parse frame header
    (void)br_read_bits(1); // Reserved bit
    (void)br_read_bits(1); // Blocking strategy
    int block_size_code = br_read_bits(4);
    int sample_rate_code = br_read_bits(4);
    int channel_mode = br_read_bits(4);
    int sample_size_code = br_read_bits(3);
    (void)br_read_bits(1); // Reserved bit

    // Coded frame/sample number (UTF-8 variable length)
    unsigned int v = br_read_bits(8);
    int num_extra = 0;
    if ((v & 0x80) == 0) { }
    else if ((v & 0xE0) == 0xC0) num_extra = 1;
    else if ((v & 0xF0) == 0xE0) num_extra = 2;
    else if ((v & 0xF8) == 0xF0) num_extra = 3;
    while (num_extra-- > 0) { (void)br_read_bits(8); }

    int block_size = 0;
    if (block_size_code == 1) block_size = 192;
    else if (block_size_code >= 2 && block_size_code <= 5) block_size = 576 << (block_size_code - 2);
    else if (block_size_code >= 8 && block_size_code <= 15) block_size = 256 << (block_size_code - 8);
    else if (block_size_code == 6) block_size = br_read_bits(8) + 1;
    else if (block_size_code == 7) block_size = br_read_bits(16) + 1;
    if (block_size <= 0 || block_size > FLAC_MAX_BLOCK_SIZE) block_size = 4096;

    if (sample_rate_code == 12) (void)br_read_bits(8);
    else if (sample_rate_code == 13 || sample_rate_code == 14) (void)br_read_bits(16);

    (void)br_read_bits(8); // CRC-8

    int bps = g_flac_info.bit_depth;
    if (sample_size_code == 1) bps = 8;
    else if (sample_size_code == 4) bps = 16;
    else if (sample_size_code == 6) bps = 24;

    // Decode subframes
    int channels = (channel_mode < 8) ? (channel_mode + 1) : 2;
    for (int ch = 0; ch < channels; ch++) {
        int subframe_bps = bps;
        if (channel_mode == 8 && ch == 1) subframe_bps++; // Side channel +1 bit
        else if (channel_mode == 9 && ch == 0) subframe_bps++;
        else if (channel_mode == 10 && ch == 1) subframe_bps++;
        decode_subframe(ch, block_size, subframe_bps);
    }

    // Align to byte
    if (g_br.bitcount & 7) {
        br_read_bits(g_br.bitcount & 7);
    }
    (void)br_read_bits(16); // Frame footer CRC-16

    // Channel decorrelation
    if (channel_mode == 8) { // Left / Side
        for (int i = 0; i < block_size; i++) {
            g_subframe_buf[1][i] = g_subframe_buf[0][i] - g_subframe_buf[1][i];
        }
    } else if (channel_mode == 9) { // Right / Side
        for (int i = 0; i < block_size; i++) {
            g_subframe_buf[0][i] = g_subframe_buf[0][i] + g_subframe_buf[1][i];
        }
    } else if (channel_mode == 10) { // Mid / Side
        for (int i = 0; i < block_size; i++) {
            int mid = g_subframe_buf[0][i];
            int side = g_subframe_buf[1][i];
            mid = (mid << 1) | (side & 1);
            g_subframe_buf[0][i] = (mid + side) >> 1;
            g_subframe_buf[1][i] = (mid - side) >> 1;
        }
    }

    unsigned int frames_to_copy = (unsigned int)block_size;
    if (frames_to_copy > max_samples) frames_to_copy = max_samples;
    short* dst = (short*)out_samples;

    for (unsigned int i = 0; i < frames_to_copy; i++) {
        int sl = g_subframe_buf[0][i];
        int sr = (channels > 1) ? g_subframe_buf[1][i] : sl;
        if (bps == 24) {
            sl >>= 8;
            sr >>= 8;
        }
        if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
        dst[i * 2]     = (short)sl;
        dst[i * 2 + 1] = (short)sr;
    }

    g_cur_sample_idx += frames_to_copy;
    if (g_flac_info.sample_rate > 0) {
        audio_set_time(g_cur_sample_idx / g_flac_info.sample_rate);
    }

    return (int)frames_to_copy;
}

int flac_dec_seek(unsigned int seconds) {
    if (!g_flac_is_open) return -1;
    if (g_flac_info.duration_s <= 0) return -1;

    unsigned int target_offset = g_first_frame_offset;
    if (seconds > 0) {
        unsigned int file_data_size = g_flac_stream.size - g_first_frame_offset;
        unsigned int approx_bytes = (seconds * file_data_size) / g_flac_info.duration_s;
        target_offset = g_first_frame_offset + approx_bytes;
        if (target_offset >= g_flac_stream.size) target_offset = g_flac_stream.size - 4096;
    }

    vfs_stream_seek(&g_flac_stream, target_offset);
    br_reset();
    g_cur_sample_idx = seconds * g_flac_info.sample_rate;
    audio_set_time(seconds);
    return 0;
}

void flac_dec_close(void) {
    if (g_flac_is_open) {
        vfs_stream_close(&g_flac_stream);
        g_flac_is_open = 0;
    }
}
