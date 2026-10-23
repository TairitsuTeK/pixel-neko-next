#ifndef SPEAKER_H
#define SPEAKER_H

void speaker_init(void);
void speaker_play(unsigned int freq, unsigned int duration_ms);
void speaker_boot_chord(void);
void speaker_shutdown_chord(void);
void speaker_reboot_chord(void);
void speaker_beep(void);

#endif // SPEAKER_H
