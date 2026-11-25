#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;

static void stop(int number)
{
    (void)number;
    stopped = 1;
}

int main(void)
{
    signal(SIGTERM, stop);
    const char *delay = getenv("VX_RECORDER_DELAY_MS");
    if (delay)
        usleep((useconds_t)atoi(delay) * 1000);
    if (stopped)
        return 0;
    if (getenv("VX_RECORDER_FAIL"))
        return 1;
    const char *trace = getenv("VX_TRACE");
    FILE *file = fopen(trace, "a");
    if (!file)
        return 1;
    fputs("recorder ready\n", file);
    fclose(file);

    int16_t samples[320] = {0}; /* 20 ms of mono PCM. */
    const char *exit_after = getenv("VX_RECORDER_EXIT_AFTER_CHUNKS");
    const char *stop_status = getenv("VX_RECORDER_STOP_STATUS");
    int chunks = 0;
    while (!stopped) {
        if (chunks == 4 && getenv("VX_TEST_WAVEFORM"))
            for (size_t i = 0; i < sizeof samples / sizeof *samples; i++)
                samples[i] = i % 2 ? INT16_MIN : INT16_MAX;
        if (write(STDOUT_FILENO, samples, sizeof samples) != sizeof samples)
            return 1;
        if (++chunks == (exit_after ? atoi(exit_after) : 0))
            return EXIT_FAILURE;
        usleep(20000);
    }
    return stop_status ? atoi(stop_status) : 0;
}
