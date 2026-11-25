#ifndef VOXFLOW_H
#define VOXFLOW_H

#include <stdbool.h>
#include <stddef.h>

enum { SAMPLE_RATE = 16000, WAV_HEADER_BYTES = 44, WAVEFORM_BARS = 31 };

enum indicator_state { RECORDING, PROCESSING };
struct indicator;
struct indicator *indicator_open(void);
bool indicator_show(struct indicator *indicator, enum indicator_state state);
bool indicator_waveform(struct indicator *indicator, const float levels[WAVEFORM_BARS]);
int indicator_fd(struct indicator *indicator);
bool indicator_dispatch(struct indicator *indicator);
void indicator_close(struct indicator *indicator);

struct transcript {
    char *text;
    size_t length;
    const char *error; /* Static user-facing text, safe to report. */
};

struct transcript transcribe(const void *wav, size_t length,
                             bool (*continue_job)(void *), void *context);

#endif
