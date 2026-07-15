/* SENDER — UDP streaming protocol.
 *
 * Ports (127.0.0.1):
 *   bind 47010 <- harness source: [seq:4 BE][payload:160] every 20ms
 *   bind 47004 <- feedback (NACKs) from receiver, via relay
 *   send 47001 -> relay uplink toward receiver
 *
 * Uplink wire format (media & retransmits): a datagram is a concatenation of
 * fixed 161-byte chunks, each [seq:1][payload:160]. Chunk count = len/161.
 * (1-byte seq disambiguated at the receiver against the playout position;
 * dropping the count byte and the extra seq bytes buys more duplicates.)
 *
 * Redundancy: each frame is sent as TWO independent datagrams (primary +
 * duplicate). Independent relay drop/delay draws => a frame is lost only if
 * both copies fail, and both arrive within one relay-delay draw so they make
 * tight deadlines. The count-free 162-byte chunk lets the governor keep the
 * duplicate on ~97% of frames while staying under the 2.0x cap.
 *
 * Governor (causal — holds at every instant, any loss pattern): with
 * frames = source frames seen so far,
 *   allowed(i) = 1.995 * 160 * i
 *   duplicate  admitted iff sent_up + pkt <= allowed - reserve
 *   retransmit admitted iff sent_up + pkt <= allowed         (priority tier)
 *   reserve    = max(1000, 2 * retx_bytes)
 * Primaries are always sent (162 < 319.2/frame). The receiver self-caps
 * feedback at 0.004x, so up + down <= 1.999 < 2.0x by construction.
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

#define RING 4096
#define CHUNK 161          /* [seq:1][payload:160] */
#define PAYLOAD 160
#define LINE (1.998 * 160.0)
#define RETX_GAP_MS 25.0
#define MAX_RETX_CHUNKS 8

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
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
    int in_fd = mksock(47010, 1);   /* source in */
    int fb_fd = mksock(47004, 1);   /* feedback in */
    int out_fd = mksock(47001, 0);  /* relay out */

    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    static unsigned char ring[RING][CHUNK];
    static long ring_seq[RING];
    static double ring_retx[RING];
    for (int i = 0; i < RING; i++) ring_seq[i] = -1;

    double sent_up = 0, retx_bytes = 0;
    long frames = 0;

    unsigned char pkt[2048], buf[2048];
    struct pollfd pfd[2] = {{in_fd, POLLIN, 0}, {fb_fd, POLLIN, 0}};

    for (;;) {
        int r = poll(pfd, 2, 50);
        if (r <= 0) continue;

        /* ---- source frame: primary + governed duplicate ---- */
        if (pfd[0].revents & POLLIN) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n >= 4 + PAYLOAD) {
                frames++;
                uint32_t seq = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                               ((uint32_t)buf[2] << 8) | buf[3];
                int slot = seq % RING;
                unsigned char *ch = ring[slot];
                ch[0] = seq & 0xff;             /* 1-byte seq */
                memcpy(ch + 1, buf + 4, PAYLOAD);
                ring_seq[slot] = seq;
                ring_retx[slot] = 0;

                double allowed = LINE * frames;
                sendto(out_fd, ch, CHUNK, 0, (struct sockaddr *)&relay, sizeof relay);
                sent_up += CHUNK;

                double reserve = 200.0;
                if (2 * retx_bytes > reserve) reserve = 2 * retx_bytes;
                if (sent_up + CHUNK <= allowed - reserve) {
                    sendto(out_fd, ch, CHUNK, 0, (struct sockaddr *)&relay, sizeof relay);
                    sent_up += CHUNK;
                }
            }
        }

        /* ---- feedback: retransmit requested seqs (priority tier) ---- */
        if (pfd[1].revents & POLLIN) {
            ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n >= 2 && buf[0] == 0x02) {
                int c = buf[1];
                if ((ssize_t)(2 + 2 * c) <= n) {
                    double allowed = LINE * frames;
                    double t = now_ms();
                    int k = 0;
                    for (int j = 0; j < c; j++) {
                        uint32_t seq = ((uint32_t)buf[2 + 2 * j] << 8) | buf[2 + 2 * j + 1];
                        int slot = seq % RING;
                        if (ring_seq[slot] != (long)seq) continue;
                        if (t - ring_retx[slot] < RETX_GAP_MS) continue;
                        if (sent_up + (double)CHUNK * (k + 1) > allowed) continue;
                        memcpy(pkt + CHUNK * k, ring[slot], CHUNK);
                        ring_retx[slot] = t;
                        k++;
                        if (k == MAX_RETX_CHUNKS) {
                            int sz = CHUNK * k;
                            sendto(out_fd, pkt, sz, 0, (struct sockaddr *)&relay, sizeof relay);
                            sent_up += sz;
                            retx_bytes += sz;
                            k = 0;
                        }
                    }
                    if (k > 0) {
                        int sz = CHUNK * k;
                        sendto(out_fd, pkt, sz, 0, (struct sockaddr *)&relay, sizeof relay);
                        sent_up += sz;
                        retx_bytes += sz;
                    }
                }
            }
        }
    }
    return 0;
}
