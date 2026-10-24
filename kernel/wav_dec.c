#include "wav_dec.h"
#include "vfs.h"
#include "audio.h"

// WAV (RIFF WAVE) Audio Decoder

static vfs_stream_t g_wav_stream;
static wav_info_t g_wav_info;
static unsigned int g_wav_bytes_per_frame = 4;
static int g_wav_is_open = 0;

static unsigned int read_le32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned short read_le16(const unsigned char* p) {
    return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

int wav_dec_open(const char* path, wav_info_t* out_info) {
    wav_dec_close();

    if (vfs_stream_open(path, &g_wav_stream) != 0) {
        return -1;
    }

    // Read 12-byte RIFF header
    unsigned char hdr[12];
    if (vfs_stream_read(&g_wav_stream, hdr, 12) < 12) {
        vfs_stream_close(&g_wav_stream);
        return -1;
    }

    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F' ||
        hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E') {
        vfs_stream_close(&g_wav_stream);
        return -1; // Not a RIFF WAVE file
    }

    int found_fmt = 0;
    int found_data = 0;
    unsigned int cur_pos = 12;

    // Scan chunks
    while (cur_pos + 8 <= g_wav_stream.size) {
        unsigned char chunk_hdr[8];
        vfs_stream_seek(&g_wav_stream, cur_pos);
        if (vfs_stream_read(&g_wav_stream, chunk_hdr, 8) < 8) break;

        unsigned int chunk_size = read_le32(chunk_hdr + 4);
        cur_pos += 8;

        if (chunk_hdr[0] == 'f' && chunk_hdr[1] == 'm' && chunk_hdr[2] == 't' && chunk_hdr[3] == ' ') {
            unsigned char fmt_data[32];
            unsigned int to_read = chunk_size < 32 ? chunk_size : 32;
            if (vfs_stream_read(&g_wav_stream, fmt_data, to_read) >= 16) {
                unsigned short format_tag = read_le16(fmt_data);
                g_wav_info.channels = read_le16(fmt_data + 2);
                g_wav_info.sample_rate = (int)read_le32(fmt_data + 4);
                g_wav_info.bit_depth = (int)read_le16(fmt_data + 14);

                if (format_tag == 3) {
                    g_wav_info.is_float = 1;
                } else if (format_tag == 0xFFFE && chunk_size >= 24) {
                    // WAVEFORMATEXTENSIBLE
                    unsigned short sub_format = read_le16(fmt_data + 24);
                    g_wav_info.is_float = (sub_format == 3);
                } else {
                    g_wav_info.is_float = 0;
                }
                found_fmt = 1;
            }
        } else if (chunk_hdr[0] == 'd' && chunk_hdr[1] == 'a' && chunk_hdr[2] == 't' && chunk_hdr[3] == 'a') {
            g_wav_info.data_offset = cur_pos;
            g_wav_info.data_size = chunk_size;
            found_data = 1;
            break;
        }

        // Align chunk size to 2 bytes
        cur_pos += (chunk_size + 1) & ~1;
    }

    if (!found_fmt || !found_data || g_wav_info.channels <= 0 || g_wav_info.sample_rate <= 0) {
        vfs_stream_close(&g_wav_stream);
        return -1;
    }

    unsigned int bytes_per_sample = g_wav_info.bit_depth / 8;
    if (bytes_per_sample == 0) bytes_per_sample = 2;
    g_wav_bytes_per_frame = bytes_per_sample * g_wav_info.channels;
    if (g_wav_bytes_per_frame == 0) g_wav_bytes_per_frame = 4;

    g_wav_info.total_samples = g_wav_info.data_size / g_wav_bytes_per_frame;
    g_wav_info.duration_s = g_wav_info.total_samples / g_wav_info.sample_rate;

    // Seek to beginning of audio data
    vfs_stream_seek(&g_wav_stream, g_wav_info.data_offset);
    g_wav_is_open = 1;

    if (out_info) {
        *out_info = g_wav_info;
    }

    // Set audio engine bitrate (bits_per_sample * sample_rate * channels / 1000)
    int kbps = (int)(((unsigned int)g_wav_info.sample_rate * g_wav_info.bit_depth * g_wav_info.channels) / 1000);
    audio_set_bitrate(kbps, 0);

    return 0;
}

int wav_dec_read_samples(void* out_samples, unsigned int max_samples) {
    if (!g_wav_is_open) return 0;

    unsigned int bytes_to_read = max_samples * g_wav_bytes_per_frame;
    unsigned int cur_data_bytes = g_wav_stream.current_offset - g_wav_info.data_offset;
    if (cur_data_bytes >= g_wav_info.data_size) return 0; // EOF

    unsigned int remaining_bytes = g_wav_info.data_size - cur_data_bytes;
    if (bytes_to_read > remaining_bytes) bytes_to_read = remaining_bytes;

    int bytes_read = vfs_stream_read(&g_wav_stream, out_samples, bytes_to_read);
    if (bytes_read <= 0) return 0;

    unsigned int frames_read = (unsigned int)bytes_read / g_wav_bytes_per_frame;

    // Update current playback time in audio engine
    unsigned int cur_sample = (g_wav_stream.current_offset - g_wav_info.data_offset) / g_wav_bytes_per_frame;
    audio_set_time(cur_sample / g_wav_info.sample_rate);

    return (int)frames_read;
}

int wav_dec_seek(unsigned int seconds) {
    if (!g_wav_is_open) return -1;
    unsigned int target_sample = seconds * g_wav_info.sample_rate;
    if (target_sample > g_wav_info.total_samples) target_sample = g_wav_info.total_samples;

    unsigned int target_offset = g_wav_info.data_offset + target_sample * g_wav_bytes_per_frame;
    vfs_stream_seek(&g_wav_stream, target_offset);
    audio_set_time(seconds);
    return 0;
}

void wav_dec_close(void) {
    if (g_wav_is_open) {
        vfs_stream_close(&g_wav_stream);
        g_wav_is_open = 0;
    }
}
