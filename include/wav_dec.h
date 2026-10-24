#ifndef WAV_DEC_H
#define WAV_DEC_H

typedef struct {
    int sample_rate;
    int channels;
    int bit_depth;
    int is_float;
    unsigned int data_offset;
    unsigned int data_size;
    unsigned int total_samples;
    int duration_s;
} wav_info_t;

int wav_dec_open(const char* path, wav_info_t* out_info);
int wav_dec_read_samples(void* out_samples, unsigned int max_samples);
int wav_dec_seek(unsigned int seconds);
void wav_dec_close(void);

#endif // WAV_DEC_H
