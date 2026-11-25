#include "voxflow.h"
#include "http.h"

#include <ctype.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char sign_in[] =
    "Codex ChatGPT credentials are missing or unreadable. Run codex login and use file-based credentials.";

static struct json_object *object_field(struct json_object *parent, const char *key,
                                        enum json_type type)
{
    struct json_object *value;
    if (json_object_object_get_ex(parent, key, &value) &&
        json_object_is_type(value, type))
        return value;
    return NULL;
}

static struct json_object *parse_json(const char *data, size_t length)
{
    struct json_tokener *parser = json_tokener_new();
    if (!parser)
        return NULL;
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *value = json_tokener_parse_ex(parser, data, (int)length);
    if (json_tokener_get_error(parser) != json_tokener_success) {
        json_object_put(value);
        value = NULL;
    }
    json_tokener_free(parser);
    return value;
}

static const char *header_value(struct json_object *value)
{
    if (!value)
        return NULL;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (length == 0 || length > 16384)
        return NULL;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)text[i] <= ' ' || (unsigned char)text[i] >= 127)
            return NULL;
    return text;
}

static struct json_object *load_auth(void)
{
    const char *codex_home = getenv("CODEX_HOME");
    const char *home = getenv("HOME");
    char *path = NULL;
    int written = -1;
    if (codex_home && *codex_home)
        written = asprintf(&path, "%s/auth.json", codex_home);
    else if (home && *home)
        written = asprintf(&path, "%s/.codex/auth.json", home);
    if (written < 0)
        return NULL;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    free(path);
    if (fd < 0)
        return NULL;

    struct stat info;
    enum { AUTH_LIMIT = 64 * 1024 };
    char data[AUTH_LIMIT + 1];
    struct json_object *auth = NULL;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        FILE *file = fdopen(fd, "r");
        if (file) {
            size_t length = fread(data, 1, sizeof data, file);
            if (!ferror(file) && length <= AUTH_LIMIT)
                auth = parse_json(data, length);
            explicit_bzero(data, length);
            fclose(file);
            return auth;
        }
    }
    close(fd);
    return NULL;
}

struct transcript transcribe(const void *wav, size_t length,
                             bool (*continue_job)(void *), void *context)
{
    struct transcript result = {0};
    struct json_object *auth = load_auth();
    struct json_object *tokens = object_field(auth, "tokens", json_type_object);
    const char *token = header_value(object_field(tokens, "access_token", json_type_string));
    const char *account = header_value(object_field(tokens, "account_id", json_type_string));
    if (!token) {
        json_object_put(auth);
        result.error = sign_in;
        return result;
    }

    struct response *response = calloc(1, sizeof *response);
    if (!response) {
        json_object_put(auth);
        result.error = "Could not allocate the transcription response.";
        return result;
    }

    if (!continue_job(context))
        goto done;
    post_audio(wav, length, token, account, continue_job, context, response);
    if (!continue_job(context))
        goto done;

    if (response->status == 401)
        result.error = "ChatGPT rejected the Codex sign-in (HTTP 401). Run codex login and try again.";
    else if (response->status == 403)
        result.error = "ChatGPT denied dictation (HTTP 403). Sign in again with codex login. If it persists, the backend may be blocking the request.";
    else if (response->status == 429)
        result.error = "ChatGPT dictation is rate-limited (HTTP 429). Try again later.";
    else if (response->status >= 500)
        result.error = "The ChatGPT dictation service failed. Try again later.";
    else if (response->error)
        result.error = response->error;
    else if (response->status < 200 || response->status >= 300)
        result.error = "ChatGPT rejected the transcription request.";
    else {
        struct json_object *payload = parse_json(response->body, response->length);
        struct json_object *value = object_field(payload, "text", json_type_string);
        if (!value) {
            result.error = "ChatGPT returned an invalid transcription response.";
        } else {
            const char *text = json_object_get_string(value);
            size_t bytes = (size_t)json_object_get_string_len(value);
            size_t first = 0;
            while (first < bytes && isspace((unsigned char)text[first]))
                first++;
            if (memchr(text, '\0', bytes))
                result.error = "ChatGPT returned an invalid transcription response.";
            else if (first == bytes)
                result.error = "ChatGPT returned an empty transcript.";
            else {
                result.text = strndup(text, bytes);
                result.length = bytes;
                if (!result.text)
                    result.error = "Could not allocate the transcript.";
            }
        }
        json_object_put(payload);
    }

done:
    free(response);
    json_object_put(auth);
    return result;
}
