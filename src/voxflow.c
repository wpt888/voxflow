#include "voxflow.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

enum {
    RECORD_SECONDS = 75,
    PCM_LIMIT = RECORD_SECONDS * SAMPLE_RATE * 2,
    MIN_PCM_BYTES = SAMPLE_RATE * 2 / 10,
    WAVEFORM_SAMPLES = SAMPLE_RATE * 25 / 1000,
    WAVEFORM_BYTES = WAVEFORM_SAMPLES * 2,
    START_TIMEOUT_MS = 3000,
    STALL_TIMEOUT_MS = 2000,
    STOP_TIMEOUT_MS = 1000,
    CLIPBOARD_TIMEOUT_MS = 3000,
    HYPRCTL_TIMEOUT_MS = 500,
    WINDOW_ID_BYTES = 24,
    LATE_PRESS_MS = 250,
};

struct job {
    int commands;
    struct indicator *indicator;
    bool released;
    bool cancelled;
    bool processing;
    const char *error;
};

static volatile sig_atomic_t interrupted;

static void interrupt(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void quiet_stdio(void)
{
    int sink = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (sink < 0)
        _exit(127);
    dup2(sink, STDERR_FILENO);
    close(sink);
}

static int report(const char *error)
{
    if (!error)
        return 0;
    fprintf(stderr, "voxflow: %s\n", error);
    syslog(LOG_ERR, "%s", error);
    if (fork() == 0) {
        quiet_stdio();
        execlp("notify-send", "notify-send", "--app-name=VoxFlow",
               "--icon=voxflow", "VoxFlow", error, (char *)NULL);
        _exit(127);
    }
    return 1;
}

static int runtime_directory(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/')
        return -1;
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!display || !*display)
        display = "wayland-0";
    /* An absolute WAYLAND_DISPLAY is also legal. Hash the full name. */
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *display; display++)
        hash = (hash ^ (unsigned char)*display) * UINT64_C(1099511628211);
    char name[32];
    snprintf(name, sizeof name, "voxflow-%016llx", (unsigned long long)hash);

    int parent = open(runtime, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0)
        return -1;
    int directory = -1;
    if (mkdirat(parent, name, 0700) == 0 || errno == EEXIST)
        directory = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    struct stat info;
    if (directory >= 0 && (fstat(directory, &info) < 0 || info.st_uid != geteuid() ||
                           (info.st_mode & 077) != 0)) {
        close(directory);
        directory = -1;
    }
    return directory;
}

static int send_command(int directory, char command)
{
    /*
     * Hyprland launches the two handlers asynchronously. Give a release that
     * starts first a short window to reach the recording process.
     */
    int64_t deadline = monotonic_ms() + LATE_PRESS_MS;
    do {
        int writer = openat(directory, "control", O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (writer >= 0) {
            ssize_t written = write(writer, &command, 1);
            int write_error = errno;
            close(writer);
            if (written == 1 || write_error == EPIPE)
                return 0;
            break;
        }
        if (errno != ENXIO && errno != ENOENT)
            break;
        if (command == 'c' || monotonic_ms() >= deadline)
            return 0;
        poll(NULL, 0, 5);
    } while (!interrupted);
    return report("Could not deliver the VoxFlow control command.");
}

static bool poll_job(struct job *job, int timeout, int audio_fd, short *audio_events)
{
    struct pollfd fds[] = {
        { .fd = job->commands, .events = POLLIN },
        { .fd = job->indicator ? indicator_fd(job->indicator) : -1, .events = POLLIN },
        { .fd = audio_fd, .events = POLLIN },
    };
    int ready = interrupted ? 0 : poll(fds, 3, timeout);
    if (interrupted)
        job->cancelled = true;
    if (ready < 0 && errno != EINTR && !job->cancelled)
        job->error = "Could not monitor the dictation session.";
    if (fds[0].revents & POLLIN) {
        char commands[128];
        ssize_t bytes = read(job->commands, commands, sizeof commands);
        if (bytes > 0) {
            job->released |= memchr(commands, 'r', (size_t)bytes) != NULL;
            job->cancelled |= memchr(commands, 'c', (size_t)bytes) != NULL;
        }
    }
    if (fds[1].revents && !job->cancelled && !indicator_dispatch(job->indicator))
        job->error = "The Wayland indicator was disconnected.";
    if (audio_events)
        *audio_events = fds[2].revents;
    return !job->cancelled && !job->error;
}

static bool continue_job(void *context)
{
    return poll_job(context, 0, -1, NULL);
}

static void tie_child_to_worker(pid_t parent)
{
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent)
        _exit(127);
    signal(SIGPIPE, SIG_DFL);
}

static bool output_pipe(int channel[2])
{
    if (pipe2(channel, O_CLOEXEC) < 0)
        return false;
    if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0) {
        close(channel[0]);
        close(channel[1]);
        return false;
    }
    return true;
}

static pid_t start_recorder(int *audio_fd)
{
    int channel[2];
    if (!output_pipe(channel))
        return -1;

    sigset_t stop, previous;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    if (sigprocmask(SIG_BLOCK, &stop, &previous) < 0) {
        close(channel[0]);
        close(channel[1]);
        return -1;
    }
    pid_t parent = getpid();
    pid_t recorder = fork();
    if (recorder == 0) {
        tie_child_to_worker(parent);
        /* A very quick release can send SIGTERM before we exec pw-record. */
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        sigprocmask(SIG_SETMASK, &previous, NULL);
        quiet_stdio();
        dup2(channel[1], STDOUT_FILENO);
        close(channel[0]);
        close(channel[1]);
        execlp("stdbuf", "stdbuf", "-o0", "pw-record", "--raw", "--rate", "16000",
               "--channels", "1", "--format", "s16", "--latency", "20ms", "-", (char *)NULL);
        _exit(127);
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);

    close(channel[1]);
    if (recorder < 0) {
        close(channel[0]);
        return -1;
    }
    *audio_fd = channel[0];
    return recorder;
}

static void kill_and_reap(pid_t child)
{
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
}

static bool await_child(struct job *job, pid_t child, int64_t deadline, int *status)
{
    for (;;) {
        pid_t result = waitpid(child, status, WNOHANG);
        if (result == child)
            return true;
        if (result < 0 && errno != EINTR)
            return false;
        if (monotonic_ms() >= deadline || !poll_job(job, 10, -1, NULL)) {
            kill_and_reap(child);
            return false;
        }
    }
}

static void hide_indicator(struct job *job)
{
    indicator_close(job->indicator);
    job->indicator = NULL;
}

static bool show_processing(struct job *job)
{
    if (!job->processing && !indicator_show(job->indicator, PROCESSING)) {
        job->error = "Could not show the Wayland processing indicator.";
        return false;
    }
    job->processing = true;
    return true;
}

static bool update_waveform(struct indicator *indicator, float levels[WAVEFORM_BARS],
                            const unsigned char *pcm, size_t length, size_t *analyzed)
{
    size_t windows = (length - *analyzed) / WAVEFORM_BYTES;
    if (windows == 0)
        return true;
    if (windows > WAVEFORM_BARS) {
        *analyzed += (windows - WAVEFORM_BARS) * WAVEFORM_BYTES;
        windows = WAVEFORM_BARS;
    }
    memmove(levels, levels + windows, (WAVEFORM_BARS - windows) * sizeof *levels);
    for (size_t bar = WAVEFORM_BARS - windows; bar < WAVEFORM_BARS; bar++) {
        uint64_t sum = 0;
        for (size_t sample = 0; sample < WAVEFORM_SAMPLES; sample++) {
            int16_t value;
            memcpy(&value, pcm + *analyzed, sizeof value);
            *analyzed += sizeof value;
            sum += (uint64_t)((int32_t)value * value);
        }
        /* Scale a quiet microphone into a useful range without altering the audio. */
        levels[bar] = fminf(1.0f, sqrtf((float)sum / WAVEFORM_SAMPLES) / 7500.0f);
    }
    return indicator_waveform(indicator, levels);
}

static size_t capture(struct job *job, unsigned char *pcm)
{
    int fd;
    pid_t recorder = start_recorder(&fd);
    if (recorder < 0) {
        job->error = "Could not start pw-record.";
        return 0;
    }

    int64_t started = monotonic_ms();
    int64_t max_end = started + RECORD_SECONDS * 1000;
    int64_t activity_end = started + START_TIMEOUT_MS;
    size_t length = 0;
    size_t waveform_analyzed = 0;
    float levels[WAVEFORM_BARS] = {0};
    bool stopping = false;
    bool eof = false;
    while (!eof) {
        int64_t deadline = activity_end < max_end ? activity_end : max_end;
        int64_t remaining = deadline - monotonic_ms();
        short audio_events = 0;
        if (!poll_job(job, remaining > 0 ? (int)remaining : 0, fd, &audio_events))
            break;
        if (job->released && !stopping) {
            if ((audio_events & (POLLHUP | POLLERR | POLLNVAL)) || kill(recorder, SIGTERM) < 0) {
                job->error = "pw-record exited during capture. Check your PipeWire input.";
                break;
            }
            stopping = true;
            max_end = activity_end = monotonic_ms() + STOP_TIMEOUT_MS;
            if (length >= 2 && !show_processing(job))
                break;
        }

        int64_t now = monotonic_ms();
        if (!stopping && (now >= max_end || length == PCM_LIMIT)) {
            job->error = "Recording reached the 75-second limit. Try a shorter dictation.";
            break;
        }
        if (now >= activity_end) {
            job->error = stopping ? "pw-record did not stop after the key was released." :
                "The microphone stopped sending audio. Check your PipeWire input.";
            break;
        }
        if (!audio_events)
            continue;

        if (length == PCM_LIMIT) {
            job->error = "Recording reached the 75-second limit. Try a shorter dictation.";
            break;
        }
        ssize_t bytes = read(fd, pcm + length, PCM_LIMIT - length);
        if (bytes == 0) {
            eof = true;
            if (!stopping)
                job->error = "pw-record exited during capture. Check your PipeWire input.";
        } else if (bytes > 0) {
            if (!stopping) {
                if (length < 2 && length + (size_t)bytes >= 2) {
                    if (!indicator_show(job->indicator, RECORDING)) {
                        job->error = "Could not show the Wayland recording indicator.";
                        break;
                    }
                    syslog(LOG_INFO, "Microphone capture started.");
                }
                activity_end = now + STALL_TIMEOUT_MS;
            }
            length += (size_t)bytes;
            if (!stopping && !update_waveform(job->indicator, levels, pcm, length, &waveform_analyzed)) {
                job->error = "Could not update the Wayland audio waveform.";
                break;
            }
        } else if (errno != EAGAIN && errno != EINTR) {
            job->error = "Could not read audio from pw-record.";
            break;
        }
    }

    close(fd);
    if (job->error || job->cancelled) {
        hide_indicator(job);
        kill_and_reap(recorder);
        return 0;
    }

    int status;
    if (!await_child(job, recorder, activity_end, &status)) {
        if (!job->error && !job->cancelled)
            job->error = "pw-record did not exit after the key was released.";
        return 0;
    }

    /* PipeWire 1.6.9 quits on SIGTERM but only reports success after a drain.
     * Accept its exit status 1 after our explicit stop and EOF on the PCM pipe. */
    if (!((WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 1)) ||
          (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM))) {
        job->error = "pw-record failed before it finished writing audio.";
        return 0;
    }

    /* A release during startup can drain audio without ever showing RECORDING. */
    length -= length % 2;
    return length;
}

static void write_le(unsigned char *destination, uint32_t value, size_t width)
{
    for (size_t byte = 0; byte < width; byte++)
        destination[byte] = (unsigned char)(value >> (8 * byte));
}

static void make_wav_header(unsigned char *wav, uint32_t length)
{
    memcpy(wav, "RIFF", 4);
    write_le(wav + 4, 36 + length, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_le(wav + 16, 16, 4);
    write_le(wav + 20, 1, 2); /* PCM */
    write_le(wav + 22, 1, 2); /* mono */
    write_le(wav + 24, SAMPLE_RATE, 4);
    write_le(wav + 28, SAMPLE_RATE * 2, 4);
    write_le(wav + 32, 2, 2);
    write_le(wav + 34, 16, 2);
    memcpy(wav + 36, "data", 4);
    write_le(wav + 40, length, 4);
}

static bool copy_transcript(struct job *job, const struct transcript *transcript)
{
    int text = memfd_create("voxflow-clipboard", MFD_CLOEXEC);
    if (text < 0)
        return false;
    if (write(text, transcript->text, transcript->length) != (ssize_t)transcript->length ||
        lseek(text, 0, SEEK_SET) < 0) {
        close(text);
        return false;
    }

    pid_t parent = getpid();
    pid_t copy = fork();
    if (copy == 0) {
        tie_child_to_worker(parent);
        quiet_stdio();
        if (dup2(text, STDIN_FILENO) < 0)
            _exit(127);
        close(text);
        execlp("wl-copy", "wl-copy", "--type", "text/plain;charset=utf-8", (char *)NULL);
        _exit(127);
    }

    close(text);
    int status;
    return copy > 0 && await_child(job, copy, monotonic_ms() + CLIPBOARD_TIMEOUT_MS, &status) &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool run_hyprctl(struct job *job, const char *command, const char *argument,
                        char *reply, size_t capacity)
{
    int channel[2];
    if (!continue_job(job) || !output_pipe(channel))
        return false;

    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0) {
        tie_child_to_worker(parent);
        quiet_stdio();
        if (dup2(channel[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(channel[0]);
        close(channel[1]);
        execlp("hyprctl", "hyprctl", command, argument, (char *)NULL);
        _exit(127);
    }
    close(channel[1]);
    if (child < 0) {
        close(channel[0]);
        return false;
    }

    size_t received = 0;
    bool complete = false;
    int64_t deadline = monotonic_ms() + HYPRCTL_TIMEOUT_MS;
    while (received + 1 < capacity) {
        int64_t remaining = deadline - monotonic_ms();
        short events = 0;
        if (remaining <= 0 || !poll_job(job, (int)remaining, channel[0], &events))
            break;
        if (!events)
            continue;
        ssize_t count = read(channel[0], reply + received, capacity - 1 - received);
        if (count > 0) {
            received += (size_t)count;
        } else if (count == 0) {
            complete = true;
            break;
        } else if (errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    close(channel[0]);
    if (!complete) {
        kill_and_reap(child);
        return false;
    }

    int status;
    if (!await_child(job, child, deadline, &status) ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    while (received > 0 && isspace((unsigned char)reply[received - 1]))
        received--;
    reply[received] = '\0';
    return true;
}

static void remember_window(struct job *job, char target[WINDOW_ID_BYTES])
{
    const char *query = "local w = hl.get_active_window(); "
                        "return w and tostring(w.stable_id or w.address) or 'none'";
    if (!run_hyprctl(job, "repl", query, target, WINDOW_ID_BYTES)) {
        target[0] = '\0';
        return;
    }

    /* The only value interpolated into Lua is a decimal ID or a hex address. */
    bool address = !strncmp(target, "0x", 2);
    const char *digits = target + (address ? 2 : 0);
    const char *allowed = address ? "0123456789abcdefABCDEF" : "0123456789";
    if (!*digits || digits[strspn(digits, allowed)] != '\0')
        target[0] = '\0';
}

static void paste_in_window(struct job *job, const char *target)
{
    char reply[64];
    if (!*target || !run_hyprctl(job, "locked", NULL, reply, sizeof reply) ||
        strcmp(reply, "false")) {
        syslog(LOG_INFO, "Automatic paste skipped; the transcript is on the clipboard.");
        return;
    }

    char script[2048];
    int written = snprintf(script, sizeof script,
        "local w = hl.get_active_window(); "
        "if not w or tostring(w.stable_id or w.address) ~= '%s' or w.accepts_input == false "
        "then return 'focus-changed' end; "
        "local terminals = { alacritty=true, kitty=true, foot=true, wezterm=true, "
        "['org.wezfurlong.wezterm']=true, ghostty=true, ['com.mitchellh.ghostty']=true, "
        "['org.codeberg.dnkl.foot']=true, ['org.gnome.terminal']=true, "
        "['org.kde.konsole']=true, konsole=true }; "
        "local terminal = terminals[(w.class or ''):lower()]; "
        "for _, tag in ipairs(w.tags or {}) do "
        "if tag == 'terminal' or tag == 'terminal*' then terminal = true end end; "
        "local mods = terminal and 'CTRL SHIFT' or 'CTRL'; "
        "local down = hl.dispatch(hl.dsp.send_key_state({mods=mods, key='V', state='down'})); "
        "local up = hl.dispatch(hl.dsp.send_key_state({mods=mods, key='V', state='up'})); "
        "return down and down.ok and up and up.ok and 'dispatched' or 'unavailable'", target);
    if (written < 0 || (size_t)written >= sizeof script ||
        !run_hyprctl(job, "repl", script, reply, sizeof reply)) {
        syslog(LOG_INFO, "Automatic paste was unavailable; the transcript is on the clipboard.");
    } else if (!strcmp(reply, "focus-changed")) {
        syslog(LOG_INFO, "Focus changed; the transcript is on the clipboard.");
    } else if (!strcmp(reply, "dispatched")) {
        syslog(LOG_INFO, "Paste shortcut sent to the focused window.");
    } else {
        syslog(LOG_INFO, "Automatic paste was unavailable; the transcript is on the clipboard.");
    }
}

static void dictate(struct job *job)
{
    if (!continue_job(job) || job->released)
        return;

    job->indicator = indicator_open();
    if (!job->indicator) {
        job->error = "Could not open the Wayland indicator. Run VoxFlow inside the Hyprland session.";
        return;
    }
    if (!continue_job(job) || job->released)
        return;

    unsigned char *wav = malloc(WAV_HEADER_BYTES + PCM_LIMIT);
    if (!wav) {
        job->error = "Could not allocate the recording buffer.";
        return;
    }

    size_t length = capture(job, wav + WAV_HEADER_BYTES);
    if (continue_job(job) && length < MIN_PCM_BYTES)
        syslog(LOG_INFO, "Recording was too short to transcribe.");
    else if (!job->error && !job->cancelled && show_processing(job)) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        for (size_t i = WAV_HEADER_BYTES; i < WAV_HEADER_BYTES + length; i += 2) {
            unsigned char first = wav[i];
            wav[i] = wav[i + 1];
            wav[i + 1] = first;
        }
#endif
        char target[WINDOW_ID_BYTES] = {0};
        remember_window(job, target);
        make_wav_header(wav, (uint32_t)length);
        syslog(LOG_INFO, "Recording released; starting transcription.");
        struct transcript transcript = transcribe(wav, WAV_HEADER_BYTES + length, continue_job, job);
        if (continue_job(job)) {
            job->error = transcript.error;
            if (transcript.text) {
                if (copy_transcript(job, &transcript)) {
                    syslog(LOG_INFO, "Transcript copied to the Wayland clipboard.");
                    paste_in_window(job, target);
                } else if (!job->cancelled && !job->error) {
                    job->error = "Could not copy the transcript with wl-copy.";
                }
            }
        }
        free(transcript.text);
    }
    free(wav);
}

static int press(int directory)
{
    int lock = openat(directory, "lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0)
        return report("Could not open the VoxFlow session lock.");
    if (flock(lock, LOCK_EX | LOCK_NB) < 0) {
        int lock_error = errno;
        close(lock);
        return lock_error == EWOULDBLOCK ? 0 : report("Could not lock the VoxFlow session.");
    }

    if (mkfifoat(directory, "control", 0600) < 0 && errno != EEXIST) {
        close(lock);
        return report("Could not create the VoxFlow control pipe.");
    }
    int commands = openat(directory, "control", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (commands < 0 || fstat(commands, &info) < 0 || !S_ISFIFO(info.st_mode)) {
        if (commands >= 0)
            close(commands);
        close(lock);
        return report("Could not open the VoxFlow control pipe.");
    }

    struct job job = { .commands = commands };
    dictate(&job);
    hide_indicator(&job);
    close(commands);
    close(lock);
    if (job.cancelled || interrupted) {
        syslog(LOG_INFO, "Dictation cancelled.");
        return 0;
    }
    return report(job.error);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: voxflow press|release|cancel");
        return 0;
    }
    if (argc != 2 || (strcmp(argv[1], "press") && strcmp(argv[1], "release") &&
                      strcmp(argv[1], "cancel"))) {
        fputs("Usage: voxflow press|release|cancel\n", stderr);
        return 2;
    }

    struct sigaction action = { .sa_handler = interrupt };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    openlog("voxflow", LOG_PID, LOG_USER);

    int directory = runtime_directory();
    if (directory < 0)
        return report("Could not open a private VoxFlow directory in XDG_RUNTIME_DIR.");
    int result = !strcmp(argv[1], "press") ? press(directory) :
                 send_command(directory, argv[1][0]);
    close(directory);
    return result;
}
