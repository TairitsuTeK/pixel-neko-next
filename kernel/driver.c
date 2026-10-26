#include "driver.h"
#include "log.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef struct {
    driver_descriptor_t descriptor;
    int state;
} driver_record_t;

static driver_record_t g_drivers[DRIVER_MAX];
static int g_driver_count;
static int g_active[5] = { -1, -1, -1, -1, -1 };

int driver_register(const driver_descriptor_t* descriptor)
{
    if (!descriptor || !descriptor->name || !descriptor->probe) return -1;
    if (g_driver_count >= DRIVER_MAX) return -2;

    for (int i = 0; i < g_driver_count; i++) {
        if (g_drivers[i].descriptor.category == descriptor->category &&
            g_drivers[i].descriptor.name == descriptor->name) {
            return 0;
        }
    }

    g_drivers[g_driver_count].descriptor = *descriptor;
    g_drivers[g_driver_count].state = DRIVER_STATE_REGISTERED;
    g_driver_count++;
    return 0;
}

int driver_probe_category(int category)
{
    if (category < 0 || category >= 5) return -1;
    if (g_active[category] >= 0) return 1;

    for (;;) {
        int candidate = -1;
        for (int i = 0; i < g_driver_count; i++) {
            if (g_drivers[i].descriptor.category != category ||
                g_drivers[i].state != DRIVER_STATE_REGISTERED) {
                continue;
            }
            if (candidate < 0 ||
                g_drivers[i].descriptor.priority >
                g_drivers[candidate].descriptor.priority) {
                candidate = i;
            }
        }
        if (candidate < 0) return 0;

        log_printf("[DRV ] probing %s\n", g_drivers[candidate].descriptor.name);
        int result = g_drivers[candidate].descriptor.probe();
        if (result > 0) {
            if (g_drivers[candidate].descriptor.start &&
                g_drivers[candidate].descriptor.start() <= 0) {
                result = 0;
            }
        }

        if (result > 0) {
            g_drivers[candidate].state = DRIVER_STATE_ACTIVE;
            g_active[category] = candidate;
            log_printf("[DRV ] active %s\n", g_drivers[candidate].descriptor.name);
            return 1;
        }

        g_drivers[candidate].state = DRIVER_STATE_FAILED;
        log_printf("[DRV ] probe failed: %s\n",
                   g_drivers[candidate].descriptor.name);
    }
}

const driver_status_t* driver_get_active(int category)
{
    if (category < 0 || category >= 5) return NULL;
    int index = g_active[category];
    if (index < 0) return NULL;

    static driver_status_t status;
    status.name = g_drivers[index].descriptor.name;
    status.category = g_drivers[index].descriptor.category;
    status.priority = g_drivers[index].descriptor.priority;
    status.state = g_drivers[index].state;
    return &status;
}

int driver_is_active(int category, const char* name)
{
    const driver_status_t* status = driver_get_active(category);
    if (!status || !name || !status->name) return 0;
    const char* a = status->name;
    const char* b = name;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void driver_deactivate(int category)
{
    if (category < 0 || category >= 5) return;
    int index = g_active[category];
    if (index >= 0 && g_drivers[index].descriptor.stop) {
        g_drivers[index].descriptor.stop();
    }
    if (index >= 0) g_drivers[index].state = DRIVER_STATE_FAILED;
    g_active[category] = -1;
}

int driver_get_count(void)
{
    return g_driver_count;
}

const driver_status_t* driver_get_status(int index)
{
    if (index < 0 || index >= g_driver_count) return NULL;
    static driver_status_t status;
    status.name = g_drivers[index].descriptor.name;
    status.category = g_drivers[index].descriptor.category;
    status.priority = g_drivers[index].descriptor.priority;
    status.state = g_drivers[index].state;
    return &status;
}
