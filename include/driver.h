#ifndef DRIVER_H
#define DRIVER_H

#define DRIVER_MAX 16

#define DRIVER_CATEGORY_DISPLAY 1
#define DRIVER_CATEGORY_AUDIO   2
#define DRIVER_CATEGORY_INPUT   3
#define DRIVER_CATEGORY_STORAGE 4

#define DRIVER_STATE_REGISTERED 0
#define DRIVER_STATE_FAILED     1
#define DRIVER_STATE_ACTIVE     2

typedef struct {
    const char* name;
    int category;
    int priority;
    int (*probe)(void);
    int (*start)(void);
    void (*stop)(void);
} driver_descriptor_t;

typedef struct {
    const char* name;
    int category;
    int priority;
    int state;
} driver_status_t;

int driver_register(const driver_descriptor_t* descriptor);
int driver_probe_category(int category);
const driver_status_t* driver_get_active(int category);
int driver_is_active(int category, const char* name);
void driver_deactivate(int category);
int driver_get_count(void);
const driver_status_t* driver_get_status(int index);

#endif
