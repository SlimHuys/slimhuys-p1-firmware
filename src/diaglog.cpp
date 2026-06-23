#include "diaglog.h"

#include <stdarg.h>

constexpr int LOG_ENTRIES = 80;
constexpr int LOG_LINE    = 120;

static char   buf[LOG_ENTRIES][LOG_LINE];
static int    head  = 0;
static int    count = 0;

void diagLog(const char* fmt, ...) {
    char line[LOG_LINE];

    // Timestamp prefix in milliseconden
    unsigned long ms = millis();
    int pfx = snprintf(line, sizeof(line), "[%lu] ", ms);

    va_list args;
    va_start(args, fmt);
    vsnprintf(line + pfx, sizeof(line) - pfx, fmt, args);
    va_end(args);

    Serial.println(line);

    strncpy(buf[head], line, LOG_LINE - 1);
    buf[head][LOG_LINE - 1] = '\0';
    head = (head + 1) % LOG_ENTRIES;
    if (count < LOG_ENTRIES) count++;
}

String getDiagLog() {
    String out;
    out.reserve(count * 60);
    int start = (count < LOG_ENTRIES) ? 0 : head;
    for (int i = 0; i < count; i++) {
        out += buf[(start + i) % LOG_ENTRIES];
        out += '\n';
    }
    return out;
}
