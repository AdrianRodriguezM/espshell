/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

#define LOGGER_QUEUE_LEN   32
#define LOGGER_LINE_MAX    256

typedef struct {
    char line[LOGGER_LINE_MAX];
} log_msg_t;

static QueueHandle_t s_q;
static volatile log_level_t s_level = LOG_INFO;
static volatile bool        s_stream = true;

static const char *level_str(log_level_t l)
{
    switch (l) {
    case LOG_ERR:     return "ERR";
    case LOG_WARN:    return "WARN";
    case LOG_INFO:    return "INFO";
    case LOG_DEBUG:   return "DEBUG";
    case LOG_VERBOSE: return "VERB";
    default:          return "?";
    }
}

void logger_init(void)
{
    if (s_q) return;
    s_q = xQueueCreate(LOGGER_QUEUE_LEN, sizeof(log_msg_t));
}

void logger_set_level(log_level_t lvl) { s_level = lvl; }
log_level_t logger_get_level(void)     { return s_level; }
void logger_set_stream(bool en)        { s_stream = en; }
bool logger_get_stream(void)           { return s_stream; }

void logger_log(log_level_t lvl, const char *tag, const char *fmt, ...)
{
    if (lvl == LOG_NONE || lvl > s_level) return;

    log_msg_t m;
    int hdr = snprintf(m.line, sizeof(m.line), "EVT LOG %s %s ",
                       level_str(lvl), tag ? tag : "-");
    if (hdr < 0 || hdr >= (int)sizeof(m.line)) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(m.line + hdr, sizeof(m.line) - hdr, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    /* Protocol invariant: one frame = one line, no embedded newlines.
     * Replace \r and \n with spaces to protect the framing layer. */
    for (char *p = m.line + hdr; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }

    /* Always mirror to UART for local debugging. */
    printf("%s\n", m.line);

    if (!s_stream || !s_q) return;
    /* Best-effort: drop the message if the queue is full, do not block. */
    (void)xQueueSend(s_q, &m, 0);
}

bool logger_drain(char *out, size_t out_sz, uint32_t wait_ms)
{
    if (!s_q || !out || out_sz == 0) return false;
    log_msg_t m;
    if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;
    strncpy(out, m.line, out_sz - 1);
    out[out_sz - 1] = '\0';
    return true;
}
