// SPDX-License-Identifier: GPL-2.0-or-later
//
// delugemu MIDI bridge — connects the emulator's raw-MIDI byte-stream chardevs
// to real host MIDI ports.
//
// QEMU chardevs are byte streams; host MIDI APIs (CoreMIDI on macOS, WinMM,
// ALSA seq) are message/packet based. The only platform-specific part of the
// bridge is the handful of calls that create the virtual ports and hand off a
// completed MIDI message; the logic that turns a serial byte stream into
// discrete MIDI messages (and flattens incoming messages back to bytes) is
// platform-agnostic and lives in plain C here, shared by every backend.
//
// One process bridges any number of transports. Each transport is given on the
// command line as NAME=SOCKET, where SOCKET is the path of a QEMU UNIX-socket
// chardev (created with `-chardev socket,...,server=on,wait=off`) and NAME is
// the label the virtual host port advertises, e.g.:
//
//   midi_bridge "DelugEmu DIN=/tmp/delugemu-din.sock" \
//               "DelugEmu USB=/tmp/delugemu-usb.sock"
//
// For each transport the bridge exposes, to host software:
//   * a virtual SOURCE  named NAME  (the Deluge's MIDI OUT — appears as a MIDI
//     input you can record from), and
//   * a virtual DESTINATION named NAME (the Deluge's MIDI IN — appears as a
//     MIDI output you can play to).
//
// The QEMU socket is full-duplex: bytes the guest transmits arrive as readable
// data (parsed into messages and pushed to the source); bytes we write to the
// socket are delivered to the guest's receiver (fed from the destination).

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Platform-agnostic MIDI stream parser.
//
// Incremental state machine that consumes a raw MIDI byte stream one byte at a
// time and invokes emit_message() for each complete message. Handles running
// status, System Real-Time bytes (0xF8-0xFF, which may interleave anywhere,
// even inside a SysEx), System Common messages and variable-length SysEx.
// ---------------------------------------------------------------------------

#define MIDI_MAX_MSG 65536 // generous SysEx ceiling (sample dumps etc.)

typedef void (*midi_emit_fn)(void *ctx, const uint8_t *msg, size_t len);

typedef struct {
    midi_emit_fn emit;
    void *ctx;

    uint8_t running_status; // last channel-voice status, 0 if none
    uint8_t msg[3];         // channel/common message assembly buffer
    int data_have;          // data bytes collected for current message
    int data_need;          // data bytes the current status expects
    int have_status;        // a channel/common status is in progress

    int in_sysex;
    uint8_t sysex[MIDI_MAX_MSG];
    size_t sysex_len;
} midi_parser;

// Number of data bytes that follow a channel-voice/system-common status byte.
static int midi_data_len(uint8_t status)
{
    if (status >= 0x80 && status <= 0xBF)
        return 2; // note off/on, poly pressure, control change
    if (status >= 0xC0 && status <= 0xDF)
        return 1; // program change, channel pressure
    if (status >= 0xE0 && status <= 0xEF)
        return 2; // pitch bend
    switch (status) {
    case 0xF1: // MTC quarter frame
    case 0xF3: // song select
        return 1;
    case 0xF2: // song position pointer
        return 2;
    default:
        return 0; // F6 tune request, F4/F5 reserved, etc.
    }
}

static void midi_parser_init(midi_parser *p, midi_emit_fn emit, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->emit = emit;
    p->ctx = ctx;
}

static void midi_parser_feed(midi_parser *p, uint8_t b)
{
    // System Real-Time: single byte, highest priority, may appear anywhere —
    // including in the middle of another message or a SysEx — without
    // disturbing the message in progress or running status.
    if (b >= 0xF8) {
        p->emit(p->ctx, &b, 1);
        return;
    }

    if (b >= 0x80) {
        // Status byte.
        if (b == 0xF0) {
            // Start of SysEx.
            p->in_sysex = 1;
            p->sysex_len = 0;
            p->sysex[p->sysex_len++] = b;
            p->running_status = 0;
            p->have_status = 0;
            return;
        }
        if (b == 0xF7) {
            // End of SysEx.
            if (p->in_sysex) {
                if (p->sysex_len < MIDI_MAX_MSG)
                    p->sysex[p->sysex_len++] = b;
                p->emit(p->ctx, p->sysex, p->sysex_len);
            }
            p->in_sysex = 0;
            p->sysex_len = 0;
            p->running_status = 0;
            p->have_status = 0;
            return;
        }

        // Any other status byte aborts an unterminated SysEx.
        p->in_sysex = 0;
        p->sysex_len = 0;

        p->msg[0] = b;
        p->data_have = 0;
        p->data_need = midi_data_len(b);
        p->have_status = 1;
        // Running status only applies to channel-voice messages (0x80-0xEF);
        // System Common (0xF1-0xF7) cancels it.
        p->running_status = (b < 0xF0) ? b : 0;

        if (p->data_need == 0) {
            // Zero-data status (e.g. F6 tune request) completes immediately.
            p->emit(p->ctx, p->msg, 1);
            p->have_status = (b < 0xF0); // keep running status alive
        }
        return;
    }

    // Data byte (b < 0x80).
    if (p->in_sysex) {
        if (p->sysex_len < MIDI_MAX_MSG)
            p->sysex[p->sysex_len++] = b;
        return;
    }

    if (!p->have_status) {
        // No status yet — try to resume via running status.
        if (p->running_status == 0)
            return; // stray data byte, ignore
        p->msg[0] = p->running_status;
        p->data_have = 0;
        p->data_need = midi_data_len(p->running_status);
        p->have_status = 1;
    }

    p->msg[1 + p->data_have] = b;
    p->data_have++;
    if (p->data_have >= p->data_need) {
        p->emit(p->ctx, p->msg, 1 + p->data_need);
        p->data_have = 0;
        // Keep running status armed for the next message; System Common
        // (running_status cleared above) ends the run.
        p->have_status = (p->running_status != 0);
    }
}

// ---------------------------------------------------------------------------
// CoreMIDI backend (macOS).
// ---------------------------------------------------------------------------

#if defined(__APPLE__)

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

typedef struct {
    char name[128];
    char path[256];
    int fd; // QEMU socket, -1 when disconnected

    MIDIEndpointRef source; // Deluge -> host
    MIDIEndpointRef dest;   // host  -> Deluge

    midi_parser parser;
} transport;

// Push one complete Deluge-originated MIDI message out the virtual source.
static void emit_to_host(void *ctx, const uint8_t *msg, size_t len)
{
    transport *t = (transport *)ctx;
    // Packet list header + payload; heap-allocate for large SysEx.
    size_t cap = len + 64;
    uint8_t stackbuf[1024 + 64];
    uint8_t *buf = (cap <= sizeof(stackbuf)) ? stackbuf : malloc(cap);
    if (!buf)
        return;

    MIDIPacketList *pktlist = (MIDIPacketList *)buf;
    MIDIPacket *pkt = MIDIPacketListInit(pktlist);
    pkt = MIDIPacketListAdd(pktlist, cap, pkt, 0 /*now*/, len, msg);
    if (pkt)
        MIDIReceived(t->source, pktlist);

    if (buf != stackbuf)
        free(buf);
}

// CoreMIDI read callback: host software played to our virtual destination.
// Flatten the packets straight to the guest's MIDI receiver over the socket.
static void host_to_deluge(const MIDIPacketList *pktlist, void *readProcRefCon,
                           void *srcConnRefCon)
{
    (void)srcConnRefCon;
    transport *t = (transport *)readProcRefCon;
    int fd = t->fd;
    if (fd < 0)
        return;

    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        const uint8_t *p = pkt->data;
        UInt16 left = pkt->length;
        while (left > 0) {
            ssize_t n = write(fd, p, left);
            if (n <= 0) {
                if (n < 0 && errno == EINTR)
                    continue;
                break; // socket gone; main loop will reconnect
            }
            p += n;
            left -= (UInt16)n;
        }
        pkt = MIDIPacketNext(pkt);
    }
}

static MIDIClientRef g_client;

static int backend_init(transport *transports, int n)
{
    OSStatus err =
        MIDIClientCreate(CFSTR("DelugEmu"), NULL, NULL, &g_client);
    if (err != noErr) {
        fprintf(stderr, "midi_bridge: MIDIClientCreate failed (%d)\n", (int)err);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        transport *t = &transports[i];
        CFStringRef cfname =
            CFStringCreateWithCString(NULL, t->name, kCFStringEncodingUTF8);

        err = MIDISourceCreate(g_client, cfname, &t->source);
        if (err != noErr)
            fprintf(stderr, "midi_bridge: MIDISourceCreate(%s) failed (%d)\n",
                    t->name, (int)err);

        err = MIDIDestinationCreate(g_client, cfname, host_to_deluge, t,
                                    &t->dest);
        if (err != noErr)
            fprintf(stderr,
                    "midi_bridge: MIDIDestinationCreate(%s) failed (%d)\n",
                    t->name, (int)err);

        CFRelease(cfname);
        midi_parser_init(&t->parser, emit_to_host, t);
    }
    return 0;
}

#else // !__APPLE__

#error "midi_bridge currently provides only a CoreMIDI (macOS) backend. " \
       "Add a WinMM or ALSA-seq backend to support this platform."

#endif

// ---------------------------------------------------------------------------
// Socket plumbing + main loop (platform-agnostic POSIX).
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

// Connect to a QEMU UNIX-socket chardev, retrying until it appears or we give
// up. QEMU may not have created the listening socket yet when we start.
static int connect_socket(const char *path)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s NAME=SOCKET [NAME=SOCKET ...]\n"
                "  Bridges QEMU UNIX-socket MIDI chardevs to host MIDI ports.\n",
                argv[0]);
        return 2;
    }

    int n = argc - 1;
    transport *transports = calloc(n, sizeof(transport));
    if (!transports)
        return 1;

    for (int i = 0; i < n; i++) {
        char *spec = argv[1 + i];
        char *eq = strchr(spec, '=');
        if (!eq) {
            fprintf(stderr, "midi_bridge: bad spec '%s' (want NAME=SOCKET)\n",
                    spec);
            return 2;
        }
        *eq = '\0';
        snprintf(transports[i].name, sizeof(transports[i].name), "%s", spec);
        snprintf(transports[i].path, sizeof(transports[i].path), "%s", eq + 1);
        transports[i].fd = -1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); // a closed socket must not kill us

    if (backend_init(transports, n) != 0)
        return 1;

    fprintf(stderr, "midi_bridge: bridging %d transport(s):\n", n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  \"%s\" <-> %s\n", transports[i].name,
                transports[i].path);

    // Poll the sockets for guest-originated bytes; the CoreMIDI read callback
    // handles the host->guest direction on its own thread.
    while (!g_stop) {
        struct pollfd pfds[64];
        int map[64];
        int nf = 0;

        for (int i = 0; i < n && nf < 64; i++) {
            if (transports[i].fd < 0) {
                transports[i].fd = connect_socket(transports[i].path);
                if (transports[i].fd < 0)
                    continue;
                fprintf(stderr, "midi_bridge: connected \"%s\"\n",
                        transports[i].name);
            }
            pfds[nf].fd = transports[i].fd;
            pfds[nf].events = POLLIN;
            pfds[nf].revents = 0;
            map[nf] = i;
            nf++;
        }

        if (nf == 0) {
            // Nothing connected yet; wait briefly and retry.
            struct timespec ts = {0, 200 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }

        int r = poll(pfds, nf, 200 /*ms*/);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int k = 0; k < nf; k++) {
            if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            transport *t = &transports[map[k]];
            uint8_t buf[1024];
            ssize_t got = read(t->fd, buf, sizeof(buf));
            if (got > 0) {
                for (ssize_t j = 0; j < got; j++)
                    midi_parser_feed(&t->parser, buf[j]);
            } else if (got == 0 || (got < 0 && errno != EINTR)) {
                // Guest closed; drop and reconnect on the next iteration.
                fprintf(stderr, "midi_bridge: \"%s\" disconnected\n", t->name);
                close(t->fd);
                t->fd = -1;
            }
        }
    }

    fprintf(stderr, "midi_bridge: shutting down\n");
    for (int i = 0; i < n; i++)
        if (transports[i].fd >= 0)
            close(transports[i].fd);
    free(transports);
    return 0;
}
