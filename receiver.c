/* RECEIVER — UDP streaming protocol.
 *
 * Ports (127.0.0.1):
 *   bind 47002 <- media/retx from sender, via relay
 *   send 47020 -> harness player: [seq:4 BE][payload:160]
 *   send 47003 -> feedback (NACKs) to sender, via relay
 *
 * Uplink datagram = concatenation of 161-byte chunks [seq:1][payload:160];
 * chunk count = len/161. The 1-byte seq is disambiguated to the full seq
 * nearest the current playout position (window << 256). No reorder buffer,
 * no playout timer: every
 * not-yet-delivered chunk is expanded to the harness format [seq:4 BE][payload]
 * and forwarded to the player the instant it arrives (first arrival counts).
 * Duplicates and stragglers hit the delivered[] bitmap and vanish.
 *
 * Backstop ARQ: every 12ms, NACK undelivered seqs that still have deadline
 * headroom. Self-suppresses at tight delays (no round-trip room), and is
 * bounded by <4 NACKs/seq, >=30ms spacing, and a 0.004x cumulative feedback
 * self-cap so the feedback lane can never breach the 2.0x overhead budget.
 * Feedback format: [0x02][count:1] then count*[seq:2 BE].
 */
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 161          /* uplink chunk: [seq:1][payload:160] */
#define PAYLOAD 160
#define TICK_MS 12.0
#define NACK_GAP_MS 30.0
#define MAX_NACK 4
#define GAP_THRESH 3
#define NACK_BUNDLE 32

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double now_epoch(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int mksock(int port, int do_bind) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (do_bind && bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        exit(1);
    }
    return fd;
}

int main(void) {
    double T0 = now_epoch(), DELAY_MS = 130, DUR = 30;
    const char *e;
    if ((e = getenv("T0"))) T0 = atof(e);
    if ((e = getenv("DELAY_MS"))) DELAY_MS = atof(e);
    if ((e = getenv("DURATION_S"))) DUR = atof(e);
    long N = (long)(DUR * 1000.0 / 20.0) + 256; /* seq range + slack */

    int in_fd = mksock(47002, 1);
    int out_fd = mksock(47020, 0);  /* player */
    int fb_fd = mksock(47003, 0);   /* feedback to relay */

    struct sockaddr_in player = {0}, relay = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47003);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char *delivered = calloc(N, 1);
    unsigned char *nackn = calloc(N, 1);
    double *lastnack = calloc(N, sizeof(double));

    long highest = -1, scan_low = 0;
    double feedback_bytes = 0;
    double next_tick = now_ms() + TICK_MS;

    unsigned char buf[2048], out[164], nack[512];
    struct pollfd pfd = {in_fd, POLLIN, 0};

    for (;;) {
        int r = poll(&pfd, 1, 5);
        if (r > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            for (long off = 0; off + CHUNK <= n; off += CHUNK) {
                /* 1-byte seq: recover full seq nearest current position */
                int H = (int)highest;
                int delta = ((int)buf[off] - (H & 0xff)) & 0xff;
                if (delta >= 128) delta -= 256;
                long seq = (long)H + delta;
                if (seq < 0 || seq >= N) continue;
                if (!delivered[seq]) {
                    delivered[seq] = 1;
                    out[0] = 0;
                    out[1] = 0;
                    out[2] = (seq >> 8) & 0xff;
                    out[3] = seq & 0xff;
                    memcpy(out + 4, buf + off + 1, PAYLOAD);
                    sendto(out_fd, out, 4 + PAYLOAD, 0,
                           (struct sockaddr *)&player, sizeof player);
                }
                if (seq > highest) highest = seq;
            }
        }

        double t = now_ms();
        if (t < next_tick) continue;
        next_tick = t + TICK_MS;
        double te = now_epoch();

        while (scan_low <= highest) {
            double dl = T0 + DELAY_MS / 1000.0 + scan_low * 0.020;
            if (delivered[scan_low] || dl < te)
                scan_low++;
            else
                break;
        }

        double frames_elapsed = (te - T0) / 0.020;
        double fb_cap = 0.004 * 160.0 * frames_elapsed;

        int k = 0;
        nack[0] = 0x02;
        for (long seq = scan_low; seq <= highest; seq++) {
            if (delivered[seq] || nackn[seq] >= MAX_NACK) continue;
            double dl = T0 + DELAY_MS / 1000.0 + seq * 0.020;
            if (dl < te + 0.045) continue;              /* no round-trip room */
            double expected = T0 + seq * 0.020;
            int gap = (highest >= seq + GAP_THRESH);
            int old = (te > expected + 0.100);
            if (!gap && !old) continue;
            if (t - lastnack[seq] < NACK_GAP_MS) continue;
            if (fb_cap > 0 && feedback_bytes + (2 + 2 * (k + 1)) > fb_cap) break;

            int p = 2 + 2 * k;
            nack[p] = (seq >> 8) & 0xff;
            nack[p + 1] = seq & 0xff;
            nackn[seq]++;
            lastnack[seq] = t;
            k++;
            if (k == NACK_BUNDLE) {
                nack[1] = k;
                int sz = 2 + 2 * k;
                sendto(fb_fd, nack, sz, 0, (struct sockaddr *)&relay, sizeof relay);
                feedback_bytes += sz;
                k = 0;
            }
        }
        if (k > 0) {
            nack[1] = k;
            int sz = 2 + 2 * k;
            sendto(fb_fd, nack, sz, 0, (struct sockaddr *)&relay, sizeof relay);
            feedback_bytes += sz;
        }
    }
    return 0;
}
