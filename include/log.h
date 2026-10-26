#ifndef LOG_H
#define LOG_H

#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

void log_init(void);
void log_write(const char* text);
void log_printf(const char* format, ...);
void log_set_level(int level);
int log_get_level(void);
int log_get_count(void);
const char* log_get_line(int index);

#endif
