#ifndef FLAC_DEC_H
#define FLAC_DEC_H

typedef struct {
    int sample_rate;
    int channels;
    int bit_depth;
    unsigned int total_samples;
    int duration_s;
    char title[64];
    char artist[64];
    char album[64];
} flac_info_t;

int flac_dec_open(const char* path, flac_info_t* out_info);
int flac_dec_read_samples(void* out_samples, unsigned int max_samples);
int flac_dec_seek(unsigned int seconds);
void flac_dec_close(void);

#endif // FLAC_DEC_H
