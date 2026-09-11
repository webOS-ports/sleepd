/* Test stub for include/internal/logging.h: log to stderr, no PmLogLib. */
#ifndef _TEST_STUB_LOGGING_H_
#define _TEST_STUB_LOGGING_H_

#include <stdio.h>
#include <stdbool.h>

#define SLEEPDLOG_DEBUG(...)    do { fprintf(stderr, "DBG: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PMLOG_TRACE(...)        do { } while (0)

/* MSGID/key based variants: swallow the msgid and kv pairs, print the tail */
#define SLEEPDLOG_WARNING(msgid, kvcount, ...)  do { fprintf(stderr, "WARN: %s\n", msgid); } while (0)
#define SLEEPDLOG_ERROR(msgid, kvcount, ...)    do { fprintf(stderr, "ERR: %s\n", msgid); } while (0)
#define SLEEPDLOG_CRITICAL(msgid, kvcount, ...) do { fprintf(stderr, "CRIT: %s\n", msgid); } while (0)
#define SLEEPDLOG_INFO(msgid, kvcount, ...)     do { fprintf(stderr, "INFO: %s\n", msgid); } while (0)

#define PMLOGKS(k, v)   (v)
#define PMLOGKFV(k, f, v) (v)

/* msgids referenced by the units under test */
#define MSGID_CONFIG_FILE_LOAD_ERR "CONFIG_FILE_LOAD_ERR"
#define PATH "PATH"

#endif
