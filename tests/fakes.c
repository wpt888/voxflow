#include "http.h"
#include "voxflow.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct indicator {
    bool visible;
    bool saw_silence;
    bool saw_voice;
};

static void trace(const char *event)
{
    FILE *file = fopen(getenv("VX_TRACE"), "a");
    assert(file);
    fprintf(file, "%s\n", event);
    fclose(file);
}

struct indicator *indicator_open(void)
{
    trace("indicator ready");
    return calloc(1, sizeof(struct indicator));
}

bool indicator_show(struct indicator *indicator, enum indicator_state state)
{
    indicator->visible = true;
    trace(state == RECORDING ? "recording" : "processing");
    return true;
}

bool indicator_waveform(struct indicator *indicator, const float levels[WAVEFORM_BARS])
{
    if (!getenv("VX_TEST_WAVEFORM"))
        return true;

    assert(indicator->visible);
    float newest = levels[WAVEFORM_BARS - 1];
    assert(newest >= 0 && newest <= 1);
    if (newest == 0 && !indicator->saw_silence) {
        indicator->saw_silence = true;
        trace("silent waveform");
    } else if (newest == 1 && !indicator->saw_voice) {
        assert(indicator->saw_silence);
        indicator->saw_voice = true;
        trace("active waveform");
    }
    return true;
}

int indicator_fd(struct indicator *indicator)
{
    (void)indicator;
    return -1;
}

bool indicator_dispatch(struct indicator *indicator)
{
    (void)indicator;
    return true;
}

void indicator_close(struct indicator *indicator)
{
    if (indicator)
        trace(indicator->visible ? "hidden" : "closed without mapping");
    free(indicator);
}

static uint32_t read_le(const unsigned char *bytes)
{
    return bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

void post_audio(const void *wav, size_t length, const char *token,
                const char *account, bool (*continue_job)(void *),
                void *context, struct response *response)
{
    const unsigned char *data = wav;
    assert(length >= WAV_HEADER_BYTES + 3200 && length <= WAV_HEADER_BYTES + 2400000);
    assert(!memcmp(data, "RIFF", 4));
    assert(read_le(data + 4) == length - 8);
    assert(!memcmp(data + 8, "WAVEfmt ", 8));
    assert(read_le(data + 20) == 0x00010001);
    assert(read_le(data + 24) == 16000);
    assert(!memcmp(data + 36, "data", 4));
    assert(read_le(data + 40) == length - WAV_HEADER_BYTES);
    assert(!strcmp(token, getenv("VX_EXPECT_TOKEN")));
    assert(account && !strcmp(account, "fake-account"));
    trace("upload");

    const char *delay = getenv("VX_HTTP_DELAY_MS");
    for (int time = delay ? atoi(delay) : 0; time > 0; time -= 10) {
        if (!continue_job(context))
            return;
        usleep(10000);
    }

    const char *status = getenv("VX_HTTP_STATUS");
    response->status = status ? strtol(status, NULL, 10) : 200;
    response->error = getenv("VX_HTTP_ERROR");
    const char *body = getenv("VX_HTTP_BODY");
    if (!body)
        body = "{\"text\":\" Gata. Café!\\n\"}";
    response->length = strlen(body);
    assert(response->length < sizeof response->body);
    memcpy(response->body, body, response->length);
}
