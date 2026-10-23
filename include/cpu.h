#ifndef CPU_H
#define CPU_H

typedef struct {
    char vendor[13];
    char brand[49];
    unsigned int family;
    unsigned int model;
    unsigned int stepping;
    unsigned int max_basic_leaf;
    unsigned int max_ext_leaf;
    unsigned int features_ecx;
    unsigned int features_edx;
    unsigned int mhz;
} cpu_info_t;

extern cpu_info_t g_cpu_info;
extern char cpu_model_string[49];

void cpu_detect(void);

#endif // CPU_H
