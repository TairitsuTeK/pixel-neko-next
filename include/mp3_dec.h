#ifndef MP3_DEC_H
#define MP3_DEC_H

typedef struct {
    int sample_rate;
    int channels;
    int bitrate_kbps;
    int is_vbr;
    int duration_s;
    unsigned int total_frames;
    char title[64];
    char artist[64];
    char album[64];
} mp3_info_t;

int mp3_dec_open(const char* path, mp3_info_t* out_info);
int mp3_dec_read_samples(void* out_samples, unsigned int max_samples);
int mp3_dec_seek(unsigned int seconds);
void mp3_dec_close(void);

#endif // MP3_DEC_H
