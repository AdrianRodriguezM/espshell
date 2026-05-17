/* SPDX-License-Identifier: GPL-3.0-or-later
 * logger.h — remote-streamed logger.
 *
 * Logs are produced via LOG_*() macros (or `esp_log` re-routed) and pushed
 * through a bounded queue. The TCP writer task drains the queue and emits
 * `EVT LOG <level> <tag> <message>` over the encrypted channel. Local UART
 * mirror is preserved for boot-time visibility.
 */
#ifndef ESPSHELL_LOGGER_H
#define ESPSHELL_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LOG_NONE    = 0,
    LOG_ERR     = 1,
    LOG_WARN    = 2,
    LOG_INFO    = 3,
    LOG_DEBUG   = 4,
    LOG_VERBOSE = 5,
} log_level_t;

void logger_init(void);

/* Set the global level (everything below it is dropped before queueing). */
void logger_set_level(log_level_t lvl);
log_level_t logger_get_level(void);

/* Enable/disable streaming over the TCP session. UART mirror is unaffected. */
void logger_set_stream(bool enable);
bool logger_get_stream(void);

/* Submit a log line. printf-style. Safe from any task. From ISR: prefer
 * deferring to a task via FreeRTOS APIs — this is not ISR-safe. */
void logger_log(log_level_t lvl, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Drain one queued message into `out` (NUL-terminated, no trailing newline).
 * Returns false if the queue is empty (and waits up to `wait_ms`). */
bool logger_drain(char *out, size_t out_sz, uint32_t wait_ms);

#define LOG_E(tag, ...) logger_log(LOG_ERR,     tag, __VA_ARGS__)
#define LOG_W(tag, ...) logger_log(LOG_WARN,    tag, __VA_ARGS__)
#define LOG_I(tag, ...) logger_log(LOG_INFO,    tag, __VA_ARGS__)
#define LOG_D(tag, ...) logger_log(LOG_DEBUG,   tag, __VA_ARGS__)
#define LOG_V(tag, ...) logger_log(LOG_VERBOSE, tag, __VA_ARGS__)

#endif /* ESPSHELL_LOGGER_H */
