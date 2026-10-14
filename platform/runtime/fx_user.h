#ifndef RUNTIME_FILEX_FX_USER_H
#define RUNTIME_FILEX_FX_USER_H

/* Project-owned FileX configuration. FileX is used with ThreadX and its
 * normal error-checking API. One ThreadX tick is 1 ms, so 10 seconds is
 * 10,000 ticks. */
// FileX consumes this configuration in preprocessor expressions.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FX_UPDATE_RATE_IN_TICKS 10000UL

/* The internal-flash volume enables FileX metadata recovery after mounting. */
#define FX_ENABLE_FAULT_TOLERANT

#endif /* RUNTIME_FILEX_FX_USER_H */
