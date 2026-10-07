#pragma once

#include <3ds.h>

// Debug log written to the SD card. Every line is flushed immediately so the
// log survives a crash or a hard power-off.
#define APP_DIR    "sdmc:/3ds/FEAEditor"
#define LOG_PATH   APP_DIR "/debug.log"

// Creates APP_DIR if needed and opens the log in append mode.
// Returns false if the SD card could not be written; logging then becomes a no-op.
bool log_init(void);
void log_close(void);

void log_write(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGI(...) log_write("INFO ", __VA_ARGS__)
#define LOGW(...) log_write("WARN ", __VA_ARGS__)
#define LOGE(...) log_write("ERROR", __VA_ARGS__)

// Logs a libctru Result with its decoded fields; returns rc so it can wrap calls:
//   if (R_FAILED(LOG_RC("FSUSER_OpenArchive", rc))) ...
Result log_result(const char *what, Result rc);
#define LOG_RC(what, rc) log_result((what), (rc))
