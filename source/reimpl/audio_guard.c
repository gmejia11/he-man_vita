/*
 * Guards for the SceAudioOut calls made by libOpenSLES (linked with -Wl,--wrap).
 *
 * libOpenSLES' audio thread fails sceAudioOutSetConfig with
 * SCE_AUDIO_OUT_ERROR_INVALID_FORMAT and keeps calling sceAudioOutOutput on that
 * port. Log the parameters, steer non-48 kHz output to a BGM port (the MAIN port
 * only accepts 48000 Hz), and never hand Vita3K audio for a misconfigured port.
 */
#include <psp2/audioout.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

int __real_sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode);
int __real_sceAudioOutSetConfig(int port, SceSize len, int freq, SceAudioOutMode mode);
int __real_sceAudioOutOutput(int port, const void *buf);

#define MAX_PORTS 16
static struct { int port, len, freq, bad; } ports[MAX_PORTS];

static int find_port(int port) {
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].port == port && ports[i].len) return i;
    return -1;
}

int __wrap_sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode) {
    SceAudioOutPortType use_type = type;
    if (type == SCE_AUDIO_OUT_PORT_TYPE_MAIN && freq != 48000)
        use_type = SCE_AUDIO_OUT_PORT_TYPE_BGM;
    int ret = __real_sceAudioOutOpenPort(use_type, len, freq, mode);
    sceClibPrintf("[audio] OpenPort(type=%d->%d, len=%d, freq=%d, mode=%d) = 0x%08X\n",
                  type, use_type, len, freq, mode, ret);
    if (ret >= 0) {
        for (int i = 0; i < MAX_PORTS; i++) {
            if (!ports[i].len) {
                ports[i].port = ret; ports[i].len = len; ports[i].freq = freq; ports[i].bad = 0;
                break;
            }
        }
    }
    return ret;
}

int __wrap_sceAudioOutSetConfig(int port, SceSize len, int freq, SceAudioOutMode mode) {
    // libOpenSLES calls SetConfig(port, -1, -1, -1) meaning "keep everything".
    // Real firmware treats -1 as "unchanged"; Vita3K rejects mode -1 (0xFF)
    // with INVALID_FORMAT. Nothing changes, so just report success.
    if ((int)len == -1 && freq == -1 && ((int)mode == -1 || ((int)mode & 0xFF) == 0xFF)) {
        sceClibPrintf("[audio] SetConfig(port=0x%X, keep all): skipped\n", port);
        return 0;
    }
    int ret = __real_sceAudioOutSetConfig(port, len, freq, mode);
    sceClibPrintf("[audio] SetConfig(port=0x%X, len=%d, freq=%d, mode=%d) = 0x%08X\n",
                  port, (int)len, freq, mode, ret);
    int i = find_port(port);
    if (i >= 0) {
        if (ret < 0) {
            ports[i].bad = 1;
        } else {
            if ((int)len > 0) ports[i].len = len;
            if (freq > 0) ports[i].freq = freq;
            ports[i].bad = 0;
        }
    }
    return ret;
}

int __wrap_sceAudioOutOutput(int port, const void *buf) {
    int i = find_port(port);
    if (port < 0 || i < 0 || ports[i].bad || !buf) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            sceClibPrintf("[audio] Output on unusable port 0x%X (buf=%p): dropping audio\n", port, buf);
        }
        // Keep the audio thread's pacing: sleep for one buffer's worth of time
        int len = (i >= 0 && ports[i].len) ? ports[i].len : 1024;
        int freq = (i >= 0 && ports[i].freq) ? ports[i].freq : 48000;
        sceKernelDelayThread((SceUInt)((long long)len * 1000000 / freq));
        return 0;
    }
    return __real_sceAudioOutOutput(port, buf);
}
