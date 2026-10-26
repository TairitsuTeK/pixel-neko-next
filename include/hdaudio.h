#ifndef HDAUDIO_H
#define HDAUDIO_H

int hdaudio_init(void); /* 0 = success, negative = failure */
int hdaudio_is_present(void);
const char* hdaudio_get_name(void);
const char* hdaudio_get_status(void);
unsigned int hdaudio_get_controller_id(void);
unsigned int hdaudio_get_codec_id(void);
unsigned short hdaudio_get_output_pin(void);

void hdaudio_set_format(int rate, int bits, int stereo);
void hdaudio_start_dma(void);
void hdaudio_stop_dma(void);
void hdaudio_pause_dma(int pause);
void hdaudio_set_volume(int master_vol, int pcm_vol);

unsigned int hdaudio_get_dma_frame_pos(void);
const void* hdaudio_get_dma_buffer(void);
void hdaudio_write_half(int half_idx, const void* data, unsigned int bytes);
void hdaudio_silence_half(int half_idx);
int hdaudio_poll_half_needed(void);

#endif
