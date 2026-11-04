#include <audio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sound.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define SOUND_CHUNK 256u
#define SOUND_FADE_SAMPLES 88u

enum sound_command_type {
    SOUND_CMD_PLAY = 1,
    SOUND_CMD_EFFECT,
    SOUND_CMD_STOP,
};

struct sound_command {
    uint32_t type;
    uint32_t count;
    uint32_t loop;
    sound_note_t notes[SOUND_MAX_NOTES];
};

_Static_assert(sizeof(struct sound_command) < 4096,
               "sound commands must fit atomically in the userspace pipe");

static pid_t sound_worker_pid = -1;
static int sound_worker_fd = -1;

static sound_note_t music_notes[SOUND_MAX_NOTES];
static size_t music_count;
static size_t music_index;
static bool music_loop;
static sound_note_t music_note;
static uint32_t music_remaining;
static uint32_t music_elapsed;
static uint32_t music_phase;
static uint32_t music_bass_phase;
static uint32_t music_overtone_phase;

static sound_note_t effect_notes[SOUND_MAX_NOTES];
static size_t effect_count;
static size_t effect_index;
static sound_note_t effect_note;
static uint32_t effect_remaining;
static uint32_t effect_elapsed;
static uint32_t effect_phase;

static uint32_t output_rate;
static uint32_t sample_remainder;
static uint64_t samples_due;
static bool audio_ready;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t duration_samples(uint16_t duration_ms)
{
    uint32_t samples = output_rate * (uint32_t)duration_ms / 1000u;

    return samples ? samples : 1;
}

static int clamp_sample(int sample)
{
    if (sample > 120)
        return 120;
    if (sample < -120)
        return -120;
    return sample;
}

static int envelope(int volume, uint32_t elapsed, uint32_t remaining)
{
    if (elapsed < SOUND_FADE_SAMPLES)
        volume = volume * (int)elapsed / (int)SOUND_FADE_SAMPLES;
    if (remaining < SOUND_FADE_SAMPLES)
        volume = volume * (int)remaining / (int)SOUND_FADE_SAMPLES;
    return volume;
}

static int square_sample(uint32_t *phase, uint16_t frequency, int volume)
{
    if (!frequency || !volume)
        return 0;
    *phase += frequency;
    if (*phase >= output_rate)
        *phase -= output_rate;
    return *phase < output_rate / 2 ? volume : -volume;
}

static int triangle_sample(uint32_t *phase, uint16_t frequency, int volume)
{
    uint32_t half = output_rate / 2;

    if (!frequency || !volume)
        return 0;
    *phase += frequency;
    if (*phase >= output_rate)
        *phase -= output_rate;
    if (*phase < half)
        return -volume + (int)((uint64_t)*phase * 2 * volume / half);
    return volume - (int)((uint64_t)(*phase - half) * 2 * volume / half);
}

static bool load_music_note(void)
{
    if (!music_count)
        return false;
    if (music_index == music_count) {
        if (!music_loop) {
            music_count = 0;
            return false;
        }
        music_index = 0;
    }

    music_note = music_notes[music_index++];
    music_remaining = duration_samples(music_note.duration_ms);
    music_elapsed = 0;
    music_phase = 0;
    music_overtone_phase = 0;
    return true;
}

static int render_music_sample(void)
{
    if (!music_remaining && !load_music_note())
        return 0;

    int sample = 0;
    if (music_note.frequency_hz) {
        int volume = envelope(48, music_elapsed, music_remaining);
        sample += square_sample(&music_phase, music_note.frequency_hz, volume);
        sample += square_sample(&music_overtone_phase,
                                (uint16_t)(music_note.frequency_hz * 2u),
                                volume / 5);
        sample += triangle_sample(&music_bass_phase, music_note.bass_hz,
                                  volume / 2);
    }
    music_elapsed++;
    music_remaining--;
    return sample;
}

static bool load_effect_note(void)
{
    if (!effect_count)
        return false;
    if (effect_index == effect_count) {
        effect_count = 0;
        return false;
    }

    effect_note = effect_notes[effect_index++];
    effect_remaining = duration_samples(effect_note.duration_ms);
    effect_elapsed = 0;
    effect_phase = 0;
    return true;
}

static int render_effect_sample(void)
{
    if (!effect_remaining && !load_effect_note())
        return 0;

    int volume = envelope(60, effect_elapsed, effect_remaining);
    int sample = triangle_sample(&effect_phase, effect_note.frequency_hz,
                                 volume);
    effect_elapsed++;
    effect_remaining--;
    return sample;
}

static void synthesize(uint8_t *buffer, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        int sample = clamp_sample(render_music_sample() + render_effect_sample());
        buffer[i] = (uint8_t)(sample + 128);
    }
}

static void accept_command(const struct sound_command *command)
{
    if (command->count > SOUND_MAX_NOTES)
        return;

    if (command->type == SOUND_CMD_PLAY) {
        for (size_t i = 0; i < command->count; i++)
            music_notes[i] = command->notes[i];
        music_count = command->count;
        music_index = 0;
        music_loop = command->loop != 0;
        music_remaining = 0;
        music_bass_phase = 0;
    } else if (command->type == SOUND_CMD_EFFECT) {
        for (size_t i = 0; i < command->count; i++)
            effect_notes[i] = command->notes[i];
        effect_count = command->count;
        effect_index = 0;
        effect_remaining = 0;
    }
}

static void sound_worker(int read_fd, unsigned int sample_rate_hz)
{
    struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
    uint8_t pending[SOUND_CHUNK];
    size_t pending_length = 0;
    size_t pending_offset = 0;
    uint64_t last = monotonic_ms();
    bool running = true;

    output_rate = sample_rate_hz;
    audio_ready = audio_set_format(sample_rate_hz) == 0;

    while (running) {
        uint64_t now = monotonic_ms();

        /* Accumulate samples due based on elapsed wall-clock time. */
        if (running && audio_ready && now > last) {
            uint64_t elapsed = now - last;
            uint64_t scaled = elapsed * output_rate + sample_remainder;
            samples_due += scaled / 1000u;
            sample_remainder = (uint32_t)(scaled % 1000u);
            last = now;
        }

        /* Non-blocking command drain: pick up play/stop commands without
         * blocking the render schedule. */
        int ready = poll(&pfd, 1, 0);
        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            struct sound_command command;
            int count = read(read_fd, &command, sizeof(command));

            if (count == 0) {
                running = false;
            } else if (count == (int)sizeof(command)) {
                if (command.type == SOUND_CMD_STOP)
                    running = false;
                else
                    accept_command(&command);
            } else if (count < 0 && errno != EINTR && errno != EAGAIN) {
                running = false;
            }
        } else if (ready < 0 && errno != EINTR) {
            running = false;
        }

        /* Render as many whole chunks as are due, then sleep until the
         * next chunk is due. This keeps the worker blocked most of the
         * time so the CPU can idle (hlt) and QEMU's UI loop gets CPU
         * time. */
        if (running && audio_ready && samples_due >= SOUND_CHUNK) {
            pending_length = SOUND_CHUNK;
            samples_due -= SOUND_CHUNK;
            synthesize(pending, pending_length);
            pending_offset = 0;

            int written = audio_write(pending, pending_length);
            if (written < 0) {
                audio_ready = false;
            } else if ((size_t)written < pending_length) {
                /* Ring full: keep the rest for next time. */
                pending_offset = (size_t)written;
                pending_length -= (size_t)written;
                samples_due += pending_length; /* re-queue unsent samples */
            } else {
                pending_length = 0;
                pending_offset = 0;
            }
        }

        /* Sleep until more samples are due or a command arrives. Cap the
         * sleep so we still poll for commands regularly. */
        if (running) {
            uint64_t sleep_ms;
            if (samples_due >= SOUND_CHUNK) {
                sleep_ms = 0; /* render immediately next iteration */
            } else if (samples_due > 0) {
                uint64_t needed = SOUND_CHUNK - samples_due;
                sleep_ms = needed * 1000u / output_rate;
                if (sleep_ms < 1)
                    sleep_ms = 1;
            } else {
                sleep_ms = (uint64_t)SOUND_CHUNK * 1000u / output_rate;
            }
            if (sleep_ms > 16)
                sleep_ms = 16;
            struct timespec ts = { sleep_ms / 1000,
                                   (sleep_ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
    }

    (void)audio_stop();
    close(read_fd);
    _exit(0);
}

static int submit_command(uint32_t type, const sound_note_t *notes,
                          size_t count, bool loop)
{
    struct sound_command command = { 0 };
    int written;

    if (sound_worker_fd < 0 || count > SOUND_MAX_NOTES || (count && !notes))
        return -1;

    command.type = type;
    command.count = (uint32_t)count;
    command.loop = loop ? 1u : 0u;
    for (size_t i = 0; i < count; i++)
        command.notes[i] = notes[i];

    written = write(sound_worker_fd, &command, sizeof(command));
    return written == (int)sizeof(command) ? 0 : -1;
}

int sound_init(unsigned int sample_rate_hz)
{
    int pipefd[2];
    pid_t pid;
    int flags;

    sound_stop();
    if (sample_rate_hz < 4000u || sample_rate_hz > 44100u || pipe(pipefd) < 0)
        return -1;

    flags = fcntl(pipefd[1], F_GETFL, 0);
    if (flags < 0 || fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[1]);
        sound_worker(pipefd[0], sample_rate_hz);
    }

    close(pipefd[0]);
    sound_worker_pid = pid;
    sound_worker_fd = pipefd[1];
    return 0;
}

int sound_play(const sound_note_t *notes, size_t count, bool loop)
{
    return submit_command(SOUND_CMD_PLAY, notes, count, loop);
}

int sound_effect(const sound_note_t *notes, size_t count)
{
    return submit_command(SOUND_CMD_EFFECT, notes, count, false);
}

void sound_stop(void)
{
    if (sound_worker_fd < 0)
        return;

    (void)submit_command(SOUND_CMD_STOP, NULL, 0, false);
    close(sound_worker_fd);
    sound_worker_fd = -1;
    if (sound_worker_pid > 0) {
        int status;
        (void)waitpid(sound_worker_pid, &status, 0);
    }
    sound_worker_pid = -1;
}
