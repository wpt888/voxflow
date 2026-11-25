#ifndef VOXFLOW_HTTP_H
#define VOXFLOW_HTTP_H

#include <stdbool.h>
#include <stddef.h>

enum { RESPONSE_LIMIT = 256 * 1024 };

struct response {
    long status;
    size_t length;
    char body[RESPONSE_LIMIT + 1];
    const char *error;
};

void post_audio(const void *wav, size_t length, const char *token,
                const char *account, bool (*continue_job)(void *),
                void *context, struct response *response);

#endif
