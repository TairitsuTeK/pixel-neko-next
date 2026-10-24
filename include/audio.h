#ifndef AUDIO_H
#define AUDIO_H

// Unified Kernel Audio Engine & DSP Processor
// Supported hardware: C-Media CMI8738/PCI (Native Hardware DMA)

typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_PLAYING = 1,
    AUDIO_STATE_PAUSED  = 2
} audio_state_t;

typedef struct {
    int sample_rate;     // Source sample rate (e.g. 44100, 48000, 96000, 192000)
    int channels;        // Source channels: 1 (Mono), 2 (Stereo)
    int bit_depth;       // Source bit depth: 8, 16, 24, 32
    int is_float;        // 1 if 32-bit IEEE Float PCM
    int target_rate;     // Target hardware rate: 44100 or 48000
    int total_duration_s;// Total length in seconds
    int current_time_s;  // Current playback time in seconds
    int bitrate_kbps;    // Bitrate in kbps (e.g. 320 for MP3, 1411 for CD WAV)
    int is_vbr;          // 1 if VBR
    char title[128];     // Track Title
} audio_info_t;

void audio_init(void);
void audio_poll(void); // Call in OS main loop to pump PCM to CMI8738 DMA

int audio_has_hardware(void);
const char* audio_get_hardware_name(void);

// High-level File Playback & Controls
int audio_play_file(const char* filepath);
void audio_pause(void);
void audio_resume(void);
void audio_stop(void);
void audio_seek_sec(unsigned int seconds);
audio_state_t audio_get_state(void);

const char* audio_get_title(void);
unsigned int audio_get_position_sec(void);
unsigned int audio_get_duration_sec(void);

// Volume & Mixer Controls (0 ~ 100)
void audio_set_volume(int vol);
int audio_get_volume(void);
void audio_toggle_mute(void);
int audio_is_muted(void);

// Audio stream information query
const audio_info_t* audio_get_info(void);

// DSP & Feed: Feed raw samples of any supported format into audio engine
// Input is converted via polyphase decimation / resampling & TPDF dither into 16-bit Stereo
int audio_feed_raw_pcm(const void* samples, unsigned int sample_count, int src_rate, int src_channels, int src_bits, int src_float);

// Clear audio buffer (e.g. on Seek / Stop)
void audio_flush_queue(void);

// Audio-Visual Spectrum (10 bands, values 0..15) & VU Meter (values 0..255)
void audio_get_spectrum(unsigned char bands[10]);
void audio_get_vu(unsigned char* left_vu, unsigned char* right_vu);

#endif // AUDIO_H
