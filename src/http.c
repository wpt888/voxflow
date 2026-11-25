#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char endpoint[] = "https://chatgpt.com/backend-api/transcribe";
static const char user_agent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36";

struct audio_part {
    const char *data;
    size_t size;
    size_t offset;
};

/* Feed curl directly from the bounded WAV buffer. */
static size_t read_audio(char *output, size_t size, size_t count, void *context)
{
    struct audio_part *audio = context;
    size_t length = audio->size - audio->offset;
    if (length > size * count)
        length = size * count;
    memcpy(output, audio->data + audio->offset, length);
    audio->offset += length;
    return length;
}

static size_t receive_response(char *input, size_t size, size_t count, void *context)
{
    struct response *response = context;
    size_t length = size * count;
    if (length > RESPONSE_LIMIT - response->length) {
        response->error = "The ChatGPT dictation response exceeded the size limit.";
        return 0;
    }
    memcpy(response->body + response->length, input, length);
    response->length += length;
    return length;
}

static bool add_header(struct curl_slist **headers, const char *key, const char *value)
{
    char *line = NULL;
    if (asprintf(&line, "%s%s", key, value) < 0)
        return false;
    struct curl_slist *updated = curl_slist_append(*headers, line);
    explicit_bzero(line, strlen(line));
    free(line);
    if (!updated)
        return false;
    *headers = updated;
    return true;
}

void post_audio(const void *wav, size_t length, const char *token,
                const char *account, bool (*continue_job)(void *),
                void *context, struct response *response)
{
    struct audio_part audio = { .data = wav, .size = length };
    struct curl_slist *headers = NULL;
    curl_mime *body = NULL;
    CURL *easy = NULL;
    CURLM *multi = NULL;
    bool registered = false;
    response->error = "Could not initialize the ChatGPT dictation request.";

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return;
    easy = curl_easy_init();
    multi = curl_multi_init();
    if (!easy || !multi)
        goto done;

    body = curl_mime_init(easy);
    curl_mimepart *file = body ? curl_mime_addpart(body) : NULL;
    if (!file || curl_mime_name(file, "file") ||
        curl_mime_filename(file, "dictation.wav") ||
        curl_mime_type(file, "audio/wav") ||
        curl_mime_data_cb(file, (curl_off_t)length, read_audio, NULL, NULL, &audio))
        goto done;

    if (!add_header(&headers, "Authorization: Bearer ", token) ||
        (account && !add_header(&headers, "ChatGPT-Account-ID: ", account)) ||
        !add_header(&headers, "Origin: ", "https://chatgpt.com") ||
        !add_header(&headers, "Referer: ", "https://chatgpt.com/") ||
        !add_header(&headers, "Accept: ", "application/json, text/plain, */*") ||
        !add_header(&headers, "Accept-Language: ", "en-US,en;q=0.9") ||
        !add_header(&headers, "Sec-CH-UA: ", "\"Google Chrome\";v=\"149\", \"Chromium\";v=\"149\", \"Not_A Brand\";v=\"24\"") ||
        !add_header(&headers, "Sec-CH-UA-Mobile: ", "?0") ||
        !add_header(&headers, "Sec-CH-UA-Platform: ", "\"macOS\"") ||
        !add_header(&headers, "Expect:", ""))
        goto done;

    if (curl_easy_setopt(easy, CURLOPT_URL, endpoint) ||
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers) ||
        curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent) ||
        curl_easy_setopt(easy, CURLOPT_MIMEPOST, body) ||
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive_response) ||
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, response) ||
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 8000L) ||
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 45000L) ||
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) ||
        curl_easy_setopt(easy, CURLOPT_PROXY, "") ||
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) ||
        curl_multi_add_handle(multi, easy) != CURLM_OK)
        goto done;
    registered = true;
    response->error = NULL;

    while (continue_job(context)) {
        int running;
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            response->error = "The ChatGPT dictation transfer failed.";
            break;
        }
        if (!running) {
            int pending;
            CURLMsg *message = curl_multi_info_read(multi, &pending);
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response->status);
            if (!response->error && (!message || message->msg != CURLMSG_DONE ||
                                     message->data.result != CURLE_OK)) {
                response->error = message && message->msg == CURLMSG_DONE &&
                                  message->data.result == CURLE_OPERATION_TIMEDOUT ?
                    "The ChatGPT dictation request timed out." :
                    "The ChatGPT dictation request failed. Check your network connection.";
            }
            break;
        }
        if (curl_multi_poll(multi, NULL, 0, 50, NULL) != CURLM_OK) {
            response->error = "The ChatGPT dictation transfer failed.";
            break;
        }
    }

done:
    if (registered)
        curl_multi_remove_handle(multi, easy);
    curl_easy_cleanup(easy);
    if (multi)
        curl_multi_cleanup(multi);
    curl_mime_free(body);
    for (struct curl_slist *entry = headers; entry; entry = entry->next)
        explicit_bzero(entry->data, strlen(entry->data));
    curl_slist_free_all(headers);
    curl_global_cleanup();
}
