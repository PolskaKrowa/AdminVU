/**
 * @file personaplex_bridge.c
 * @brief Implementation of the Discord Voice <-> PersonaPlex audio bridge.
 *
 * See personaplex_bridge.h for the full design overview and usage guide.
 *
 */

#include "personaplex_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

#include <pthread.h>
#include <unistd.h>

/* POSIX networking */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* libsodium -- Discord encryption */
#include <sodium.h>

/* libwebsockets -- PersonaPlex WebSocket client */
#include <libwebsockets.h>

/* =========================================================================
 * Internal constants
 * ========================================================================= */

/** Discord RTP headers are always exactly 12 bytes. */
#define RTP_HEADER_LEN 12

/**
 * crypto_secretbox_easy() prepends a 16-byte Poly1305 MAC to the ciphertext.
 * We need to account for this when sizing receive and send buffers.
 */
#define DISCORD_MAC_LEN  crypto_secretbox_MACBYTES   /* 16 */

/**
 * xsalsa20_poly1305 nonce is 24 bytes.
 * Discord constructs it as: [12-byte RTP header] || [12 zero bytes].
 */
#define NONCE_LEN 24

/**
 * Maximum size of a UDP datagram we accept from Discord.
 * = RTP header + MAC + max Opus payload.
 */
#define UDP_RECV_BUF (RTP_HEADER_LEN + DISCORD_MAC_LEN + PPX_MAX_OPUS_FRAME)

/**
 * Maximum size of a UDP datagram we send to Discord.
 * Same layout as above.
 */
#define UDP_SEND_BUF UDP_RECV_BUF

/**
 * Discord UDP IP discovery packet is exactly 74 bytes (as of voice gateway v8).
 * Layout:
 *   [0-1]  : 0x00 0x01  (request type)
 *   [2-3]  : 0x00 0x46  (payload length = 70, big-endian)
 *   [4-7]  : ssrc       (big-endian uint32)
 *   [8-71] : zero-padded (address field in response)
 *   [72-73]: 0x00 0x00  (port field, filled by server in response)
 */
#define IP_DISCOVERY_LEN 74

/** How long to wait for Discord's IP discovery response. */
#define IP_DISCOVERY_TIMEOUT_S 5

/**
 * RTP timestamp increment per 20 ms frame at 48 kHz.
 * 48000 samples/sec * 0.020 sec = 960 samples.
 * If your bot sends frames at a different interval, adjust accordingly.
 */
#define RTP_TIMESTAMP_STEP 960

/* =========================================================================
 * RTP header layout
 * =========================================================================
 *
 * Discord voice uses standard RTP (RFC 3550) with:
 *   flags        = 0x80  (V=2, P=0, X=0, CC=0)
 *   payload_type = 0x78  (M=0, PT=120 — Discord's Opus payload type)
 *   sequence     : big-endian uint16, incremented per packet
 *   timestamp    : big-endian uint32, incremented by RTP_TIMESTAMP_STEP
 *   ssrc         : big-endian uint32, assigned by Discord in READY
 */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  payload_type;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_header_t;

/* =========================================================================
 * Thread-safe ring buffer for Opus frames
 * =========================================================================
 *
 * Both directions (discord->ppx and ppx->discord) use this structure.
 * Frames are fixed-size slots; we copy in and out.
 * When full, push() drops the incoming frame rather than blocking, so
 * the network receive loop is never stalled.
 */

struct opus_frame {
    uint8_t data[PPX_MAX_OPUS_FRAME];
    size_t  len;
};

struct frame_queue {
    struct opus_frame slots[PPX_QUEUE_CAPACITY];
    int    head;    /**< Index of next slot to read. */
    int    tail;    /**< Index of next slot to write. */
    int    count;   /**< Number of frames currently held. */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty; /**< Signalled when a frame is pushed. */
};

static void
queue_init(struct frame_queue *q)
{
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->lock,      NULL);
    pthread_cond_init (&q->not_empty, NULL);
}

static void
queue_destroy(struct frame_queue *q)
{
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy (&q->not_empty);
}

/**
 * queue_push - copy a frame into the queue.
 *
 * Non-blocking. Returns false (and drops the frame) if the queue is full.
 * The caller should log a warning in this case; it indicates the consumer
 * thread is falling behind.
 */
static bool
queue_push(struct frame_queue *q, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&q->lock);

    if (q->count == PPX_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&q->lock);
        return false; /* full — caller logs and drops */
    }

    q->slots[q->tail].len = len;
    memcpy(q->slots[q->tail].data, data, len);
    q->tail  = (q->tail + 1) % PPX_QUEUE_CAPACITY;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return true;
}

/**
 * queue_pop - block until a frame is available, then copy it out.
 *
 * Uses a 50 ms timed wait so the caller can periodically check the
 * running flag without spinning.
 *
 * @param running  Checked on each timeout; pop returns false when false.
 * @return true if a frame was copied into dst, false if shutting down.
 */
static bool
queue_pop(struct frame_queue *q, struct opus_frame *dst, atomic_bool *running)
{
    pthread_mutex_lock(&q->lock);

    while (q->count == 0 && atomic_load(running)) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 50L * 1000L * 1000L; /* 50 ms */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&q->not_empty, &q->lock, &deadline);
    }

    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    *dst    = q->slots[q->head];
    q->head = (q->head + 1) % PPX_QUEUE_CAPACITY;
    q->count--;

    pthread_mutex_unlock(&q->lock);
    return true;
}

/**
 * queue_try_pop - non-blocking pop.
 *
 * Returns false immediately if the queue is empty.
 * Used by the lws WRITEABLE callback to drain without blocking.
 */
static bool
queue_try_pop(struct frame_queue *q, struct opus_frame *dst)
{
    pthread_mutex_lock(&q->lock);

    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    *dst    = q->slots[q->head];
    q->head = (q->head + 1) % PPX_QUEUE_CAPACITY;
    q->count--;

    pthread_mutex_unlock(&q->lock);
    return true;
}

/* =========================================================================
 * Bridge state (internal)
 * ========================================================================= */

struct ppx_bridge {
    struct ppx_bridge_config cfg;

    /* UDP socket used for all Discord voice traffic. */
    int udp_fd;

    /* Resolved Discord voice server address (set once at start). */
    struct sockaddr_in discord_addr;

    /*
     * Our external IP and port discovered via Discord's IP discovery.
     * You need these if you want to send SELECT PROTOCOL (op 1) yourself;
     * this module discovers them but doesn't send op 1 — that's orca's job
     * and must happen before the secret key is known.
     *
     * Exposed here in case the caller needs them after ppx_bridge_start().
     */
    char     our_ip[64];
    uint16_t our_port;

    /* Rolling RTP state for packets we send to Discord. */
    uint16_t rtp_seq;   /**< Incremented by 1 per sent packet. */
    uint32_t rtp_ts;    /**< Incremented by RTP_TIMESTAMP_STEP per sent packet. */

    /* libwebsockets context and active connection handle. */
    struct lws_context *lws_ctx;
    struct lws         *lws_wsi; /**< NULL when not connected. */

    /*
     * Discord → PersonaPlex direction:
     * udp_rx_thread fills this; lws WRITEABLE callback drains it.
     */
    struct frame_queue discord_to_ppx;

    /*
     * PersonaPlex → Discord direction:
     * lws RECEIVE callback fills this; udp_tx_thread drains it.
     */
    struct frame_queue ppx_to_discord;

    /** Set to false to signal all threads to stop. */
    atomic_bool running;

    pthread_t udp_rx_thread;
    pthread_t lws_service_thread;
    pthread_t udp_tx_thread;
};

/* =========================================================================
 * Crypto helpers
 * ========================================================================= */

/**
 * build_nonce - construct the 24-byte xsalsa20_poly1305 nonce.
 *
 * Discord's nonce = [12-byte RTP header] || [12 zero bytes].
 * This is fixed for xsalsa20_poly1305; newer Discord modes (aead_xchacha20_
 * poly1305_rtpsize) use a different nonce construction — see Discord docs if
 * you need to support those.
 */
static void
build_nonce(uint8_t nonce[NONCE_LEN], const rtp_header_t *hdr)
{
    memcpy(nonce,              hdr, RTP_HEADER_LEN);
    memset(nonce + RTP_HEADER_LEN, 0, NONCE_LEN - RTP_HEADER_LEN);
}

/**
 * decrypt_discord_packet - decrypt an incoming Discord UDP voice packet.
 *
 * Input packet layout:  [12-byte RTP header][16-byte MAC][encrypted Opus]
 * Output:               raw Opus bytes written to `out`, length to `out_len`.
 *
 * Returns false if the packet is malformed or the MAC fails (wrong key,
 * replay, corruption). The caller should silently discard such packets.
 */
static bool
decrypt_discord_packet(const struct ppx_bridge *b,
                       const uint8_t *pkt, size_t pkt_len,
                       uint8_t *out, size_t *out_len)
{
    if (pkt_len <= (size_t)(RTP_HEADER_LEN + DISCORD_MAC_LEN)) {
        return false; /* too short to be a valid voice packet */
    }

    const rtp_header_t *hdr = (const rtp_header_t *)pkt;
    uint8_t nonce[NONCE_LEN];
    build_nonce(nonce, hdr);

    const uint8_t *cipher     = pkt + RTP_HEADER_LEN;
    size_t         cipher_len = pkt_len - RTP_HEADER_LEN;

    if (crypto_secretbox_open_easy(out, cipher, cipher_len,
                                   nonce, b->cfg.discord_secret_key) != 0) {
        /*
         * Decryption failed. This can happen legitimately (e.g. Discord sends
         * RTCP control packets on the same socket). Don't log every failure.
         */
        return false;
    }

    *out_len = cipher_len - DISCORD_MAC_LEN;
    return true;
}

/**
 * build_and_encrypt_discord_packet - construct a ready-to-send Discord UDP packet.
 *
 * Builds the RTP header (advancing rtp_seq and rtp_ts), encrypts the Opus
 * payload with xsalsa20_poly1305, and writes the full packet to `out`.
 *
 * `out` must be at least RTP_HEADER_LEN + DISCORD_MAC_LEN + opus_len bytes.
 *
 * Returns the total number of bytes written to `out`.
 */
static size_t
build_and_encrypt_discord_packet(struct ppx_bridge *b,
                                 const uint8_t *opus, size_t opus_len,
                                 uint8_t *out)
{
    rtp_header_t hdr = {
        .flags        = 0x80,
        .payload_type = 0x78,
        .sequence     = htons(b->rtp_seq++),
        .timestamp    = htonl(b->rtp_ts),
        .ssrc         = htonl(b->cfg.discord_ssrc),
    };
    b->rtp_ts += RTP_TIMESTAMP_STEP;

    memcpy(out, &hdr, RTP_HEADER_LEN);

    uint8_t nonce[NONCE_LEN];
    build_nonce(nonce, &hdr);

    /* crypto_secretbox_easy writes DISCORD_MAC_LEN bytes of MAC followed
     * by the ciphertext, all starting at out + RTP_HEADER_LEN. */
    crypto_secretbox_easy(out + RTP_HEADER_LEN,
                          opus, opus_len,
                          nonce, b->cfg.discord_secret_key);

    return (size_t)(RTP_HEADER_LEN + DISCORD_MAC_LEN + opus_len);
}

/* =========================================================================
 * Discord UDP IP discovery
 * =========================================================================
 *
 * Must be called before the UDP socket is used for voice traffic.
 * Discord expects a 74-byte discovery packet and responds with our external
 * IP (as a null-terminated string at bytes 8-71) and port (little-endian
 * uint16 at bytes 72-73).
 *
 * Returns 0 on success, -1 on error.
 */
static int
perform_ip_discovery(struct ppx_bridge *b)
{
    uint8_t req[IP_DISCOVERY_LEN] = {0};

    req[0] = 0x00; req[1] = 0x01;  /* request type */
    req[2] = 0x00; req[3] = 0x46;  /* payload length = 70 */
    uint32_t ssrc_be = htonl(b->cfg.discord_ssrc);
    memcpy(req + 4, &ssrc_be, 4);

    if (sendto(b->udp_fd, req, sizeof(req), 0,
               (struct sockaddr *)&b->discord_addr,
               sizeof(b->discord_addr)) < 0) {
        perror("[ppx_bridge] IP discovery sendto");
        return -1;
    }

    /* Apply a receive timeout so we don't block forever. */
    struct timeval tv = { .tv_sec = IP_DISCOVERY_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(b->udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t resp[IP_DISCOVERY_LEN] = {0};
    ssize_t n = recvfrom(b->udp_fd, resp, sizeof(resp), 0, NULL, NULL);

    /* Remove the timeout before returning. */
    tv.tv_sec = 0;
    setsockopt(b->udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (n < 0) {
        perror("[ppx_bridge] IP discovery recvfrom (timed out?)");
        return -1;
    }

    /* Extract our external IP (null-terminated at offset 8). */
    strncpy(b->our_ip, (const char *)(resp + 8), sizeof(b->our_ip) - 1);

    /* Extract our external port (little-endian uint16 at offset 72). */
    b->our_port = (uint16_t)(resp[72]) | ((uint16_t)(resp[73]) << 8);

    printf("[ppx_bridge] IP discovery complete: external address %s:%u\n",
           b->our_ip, b->our_port);
    return 0;
}

/* =========================================================================
 * UDP receive thread  (Discord → PersonaPlex)
 * ========================================================================= */

static void *
udp_rx_thread_fn(void *arg)
{
    struct ppx_bridge *b = (struct ppx_bridge *)arg;

    uint8_t raw_pkt[UDP_RECV_BUF];
    uint8_t opus[PPX_MAX_OPUS_FRAME];

    printf("[ppx_bridge] UDP receive thread started\n");

    while (atomic_load(&b->running)) {
        ssize_t n = recvfrom(b->udp_fd, raw_pkt, sizeof(raw_pkt),
                             0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;           /* signal, retry */
            if (!atomic_load(&b->running)) break;   /* socket was closed by stop() */
            perror("[ppx_bridge] recvfrom");
            break;
        }

        /* Verify this looks like RTP (version bits in top 2 bits = 0b10). */
        if (n < RTP_HEADER_LEN || (raw_pkt[0] >> 6) != 2)
            continue;

        size_t opus_len = 0;
        if (!decrypt_discord_packet(b, raw_pkt, (size_t)n, opus, &opus_len))
            continue; /* bad MAC or RTCP control packet — discard silently */

        if (!queue_push(&b->discord_to_ppx, opus, opus_len)) {
            fprintf(stderr, "[ppx_bridge] discord->ppx queue full, dropping frame\n");
            continue;
        }

        /*
         * Wake the libwebsockets service loop so it sends this frame
         * promptly. lws_cancel_service() is documented as thread-safe
         * and is the correct way to interrupt lws_service() from another
         * thread.
         */
        if (b->lws_ctx)
            lws_cancel_service(b->lws_ctx);
    }

    printf("[ppx_bridge] UDP receive thread exited\n");
    return NULL;
}

/* =========================================================================
 * UDP transmit thread  (PersonaPlex → Discord)
 * ========================================================================= */

static void *
udp_tx_thread_fn(void *arg)
{
    struct ppx_bridge *b = (struct ppx_bridge *)arg;

    struct opus_frame frame;
    uint8_t out[UDP_SEND_BUF];

    printf("[ppx_bridge] UDP transmit thread started\n");

    while (queue_pop(&b->ppx_to_discord, &frame, &b->running)) {
        size_t pkt_len = build_and_encrypt_discord_packet(
                             b, frame.data, frame.len, out);

        if (sendto(b->udp_fd, out, pkt_len, 0,
                   (struct sockaddr *)&b->discord_addr,
                   sizeof(b->discord_addr)) < 0) {
            if (!atomic_load(&b->running)) break;
            perror("[ppx_bridge] sendto Discord");
            /* Non-fatal: keep trying. A persistent error means the socket
             * was closed, at which point running will be false shortly. */
        }
    }

    printf("[ppx_bridge] UDP transmit thread exited\n");
    return NULL;
}

/* =========================================================================
 * libwebsockets protocol callbacks
 * =========================================================================
 *
 * The bridge pointer is stored as the lws context user data so we can
 * access it from callbacks without a global variable.
 */

static int
lws_bridge_callback(struct lws *wsi,
                    enum lws_callback_reasons reason,
                    void *user,
                    void *in, size_t len)
{
    /*
     * Retrieve our bridge from the lws context user data.
     * We store it there in ppx_bridge_start() via ctx_info.user.
     */
    struct ppx_bridge *b =
        (struct ppx_bridge *)lws_context_user(lws_get_context(wsi));

    (void)user;

    switch (reason) {

    /* ------------------------------------------------------------------
     * Connection established: send the PersonaPlex handshake immediately.
     * ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("[ppx_bridge] WebSocket connected to PersonaPlex\n");
        b->lws_wsi = wsi;

        {
            /*
             * PersonaPlex handshake: [0x00][version][model]
             * lws_write() requires LWS_PRE bytes of blank space before the
             * payload so libwebsockets can write the WebSocket frame header.
             */
            uint8_t buf[LWS_PRE + 3];
            memset(buf, 0, LWS_PRE);
            buf[LWS_PRE + 0] = PPX_MSG_HANDSHAKE;
            buf[LWS_PRE + 1] = PPX_PROTOCOL_VERSION;
            buf[LWS_PRE + 2] = PPX_PROTOCOL_MODEL;
            lws_write(wsi, buf + LWS_PRE, 3, LWS_WRITE_BINARY);
        }
        break;

    /* ------------------------------------------------------------------
     * Writable: drain the discord->ppx queue and send one frame.
     *
     * We send one Opus frame per WRITEABLE callback and re-request
     * WRITEABLE if more frames are waiting. This keeps the lws event
     * loop responsive and prevents us from blocking it.
     * ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        struct opus_frame frame;
        if (!queue_try_pop(&b->discord_to_ppx, &frame))
            break; /* nothing to send right now */

        /*
         * PersonaPlex audio message: [0x01][raw Opus frame]
         * Buffer must have LWS_PRE bytes of headroom before our data.
         */
        uint8_t buf[LWS_PRE + 1 + PPX_MAX_OPUS_FRAME];
        memset(buf, 0, LWS_PRE);
        buf[LWS_PRE] = PPX_MSG_AUDIO;
        memcpy(buf + LWS_PRE + 1, frame.data, frame.len);

        lws_write(wsi, buf + LWS_PRE, 1 + frame.len, LWS_WRITE_BINARY);

        /* If more frames are waiting, schedule another WRITEABLE callback. */
        if (b->discord_to_ppx.count > 0)
            lws_callback_on_writable(wsi);
        break;
    }

    /* ------------------------------------------------------------------
     * Received a message from PersonaPlex.
     *
     * All audio messages start with 0x01. Other message types (text
     * transcriptions, control messages) are ignored here but you can
     * add handling below if needed.
     * ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        if (len < 2) break;

        const uint8_t *msg = (const uint8_t *)in;

        if (msg[0] != PPX_MSG_AUDIO) {
            /*
             * Received a non-audio message from PersonaPlex (e.g. 0x02 text).
             * You can handle transcripts or other signals here if desired.
             */
            break;
        }

        const uint8_t *opus_data = msg + 1;
        size_t         opus_len  = len - 1;

        if (!queue_push(&b->ppx_to_discord, opus_data, opus_len)) {
            fprintf(stderr, "[ppx_bridge] ppx->discord queue full, dropping frame\n");
        }
        break;
    }

    /* ------------------------------------------------------------------
     * lws_cancel_service() was called from udp_rx_thread.
     * Check the queue and request WRITEABLE if there is work to do.
     * ------------------------------------------------------------------ */
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        if (b->lws_wsi && b->discord_to_ppx.count > 0)
            lws_callback_on_writable(b->lws_wsi);
        break;

    /* ------------------------------------------------------------------
     * Error / close handling.
     * ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "[ppx_bridge] WebSocket connection error: %s\n",
                in ? (const char *)in : "(unknown)");
        b->lws_wsi = NULL;
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        printf("[ppx_bridge] WebSocket connection closed\n");
        b->lws_wsi = NULL;
        break;

    default:
        break;
    }

    return 0;
}

static const struct lws_protocols lws_protocols[] = {
    {
        .name                  = "personaplex-bridge",
        .callback              = lws_bridge_callback,
        .per_session_data_size = 0,
        .rx_buffer_size        = 0,    /* let lws choose a sensible default */
    },
    { NULL, NULL, 0, 0 }               /* required terminator */
};

/* =========================================================================
 * libwebsockets service thread
 * ========================================================================= */

static void *
lws_service_thread_fn(void *arg)
{
    struct ppx_bridge *b = (struct ppx_bridge *)arg;

    printf("[ppx_bridge] WebSocket service thread started\n");

    /*
     * lws_service() runs the event loop for up to `timeout_ms` milliseconds
     * before returning. We use 50 ms so we can check the running flag
     * without burning CPU. lws_cancel_service() from udp_rx_thread will
     * interrupt this early whenever there is audio to send.
     */
    while (atomic_load(&b->running)) {
        lws_service(b->lws_ctx, 50);
    }

    printf("[ppx_bridge] WebSocket service thread exited\n");
    return NULL;
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

struct ppx_bridge *
ppx_bridge_create(const struct ppx_bridge_config *cfg)
{
    /* libsodium must be initialised before any crypto calls.
     * sodium_init() is idempotent and safe to call multiple times. */
    if (sodium_init() < 0) {
        fprintf(stderr, "[ppx_bridge] libsodium initialisation failed\n");
        return NULL;
    }

    struct ppx_bridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->cfg    = *cfg;
    b->udp_fd = -1;
    atomic_init(&b->running, false);

    queue_init(&b->discord_to_ppx);
    queue_init(&b->ppx_to_discord);

    /*
     * Seed RTP sequence number and timestamp with cryptographically
     * random values, as recommended by RFC 3550 §5.1.
     */
    randombytes_buf(&b->rtp_seq, sizeof(b->rtp_seq));
    randombytes_buf(&b->rtp_ts,  sizeof(b->rtp_ts));

    return b;
}

int
ppx_bridge_start(struct ppx_bridge *b)
{
    /* ----------------------------------------------------------------
     * 1. Create and bind the UDP socket.
     * ---------------------------------------------------------------- */

    b->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (b->udp_fd < 0) {
        perror("[ppx_bridge] socket");
        return -1;
    }

    /* Resolve the Discord voice server address once. */
    memset(&b->discord_addr, 0, sizeof(b->discord_addr));
    b->discord_addr.sin_family      = AF_INET;
    b->discord_addr.sin_port        = htons(b->cfg.discord_server_port);
    b->discord_addr.sin_addr.s_addr = inet_addr(b->cfg.discord_server_ip);

    /* Bind to INADDR_ANY on a random OS-assigned port. */
    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_port        = 0,             /* OS picks the port */
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(b->udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("[ppx_bridge] bind");
        goto fail_socket;
    }

    /* ----------------------------------------------------------------
     * 2. Perform Discord UDP IP discovery.
     *
     * This is required so Discord knows our external IP and port and
     * can route voice packets back to us. The result is stored in
     * b->our_ip and b->our_port for the caller's reference.
     * ---------------------------------------------------------------- */

    if (perform_ip_discovery(b) < 0) {
        fprintf(stderr, "[ppx_bridge] IP discovery failed\n");
        goto fail_socket;
    }

    /* ----------------------------------------------------------------
     * 3. Set up the libwebsockets context and connect to PersonaPlex.
     * ---------------------------------------------------------------- */

    /*
     * Pass the bridge pointer as context user data so our callback can
     * retrieve it without needing a global variable.
     */
    struct lws_context_creation_info ctx_info = {
        .port      = CONTEXT_PORT_NO_LISTEN,   /* client only — no server */
        .protocols = lws_protocols,
        .options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT,
        .user      = b,                        /* retrieved in callbacks */
    };

    b->lws_ctx = lws_create_context(&ctx_info);
    if (!b->lws_ctx) {
        fprintf(stderr, "[ppx_bridge] failed to create lws context\n");
        goto fail_socket;
    }

    /* Initiate the WebSocket connection. The actual TCP handshake and
     * WebSocket upgrade happen asynchronously inside lws_service(). */
    struct lws_client_connect_info conn_info = {
        .context        = b->lws_ctx,
        .address        = b->cfg.personaplex_host,
        .port           = b->cfg.personaplex_port,
        .path           = b->cfg.personaplex_path,
        .host           = b->cfg.personaplex_host,
        .origin         = b->cfg.personaplex_host,
        .protocol       = lws_protocols[0].name,
        .ssl_connection = b->cfg.personaplex_use_tls
            ? (b->cfg.personaplex_skip_cert_verify
                ? LCCSCF_USE_SSL
                  | LCCSCF_ALLOW_SELFSIGNED
                  | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK
                : LCCSCF_USE_SSL)
            : 0,
    };

    b->lws_wsi = lws_client_connect_via_info(&conn_info);
    if (!b->lws_wsi) {
        fprintf(stderr, "[ppx_bridge] failed to initiate WebSocket connection "
                        "to %s:%u%s\n",
                b->cfg.personaplex_host, b->cfg.personaplex_port,
                b->cfg.personaplex_path);
        goto fail_lws;
    }

    /* ----------------------------------------------------------------
     * 4. Start the three bridge threads.
     * ---------------------------------------------------------------- */

    atomic_store(&b->running, true);

    if (pthread_create(&b->lws_service_thread, NULL, lws_service_thread_fn, b)) {
        perror("[ppx_bridge] pthread_create (lws_service_thread)");
        goto fail_running;
    }
    if (pthread_create(&b->udp_rx_thread, NULL, udp_rx_thread_fn, b)) {
        perror("[ppx_bridge] pthread_create (udp_rx_thread)");
        goto fail_running;
    }
    if (pthread_create(&b->udp_tx_thread, NULL, udp_tx_thread_fn, b)) {
        perror("[ppx_bridge] pthread_create (udp_tx_thread)");
        goto fail_running;
    }

    printf("[ppx_bridge] bridge running: Discord %s:%u <-> PersonaPlex %s:%u%s\n",
           b->cfg.discord_server_ip,  b->cfg.discord_server_port,
           b->cfg.personaplex_host,   b->cfg.personaplex_port,
           b->cfg.personaplex_path);
    return 0;

fail_running:
    atomic_store(&b->running, false);
fail_lws:
    lws_context_destroy(b->lws_ctx);
    b->lws_ctx = NULL;
    b->lws_wsi = NULL;
fail_socket:
    close(b->udp_fd);
    b->udp_fd = -1;
    return -1;
}

void
ppx_bridge_stop(struct ppx_bridge *b)
{
    if (!b) return;

    printf("[ppx_bridge] stopping...\n");

    /* Signal all threads to exit. */
    atomic_store(&b->running, false);

    /*
     * Unblock udp_rx_thread: close the socket so that recvfrom() returns
     * with an error. The thread checks the running flag after any error
     * and will exit cleanly.
     */
    if (b->udp_fd >= 0) {
        close(b->udp_fd);
        b->udp_fd = -1;
    }

    /*
     * Unblock udp_tx_thread: broadcast on the queue condition variable
     * so queue_pop() can observe running == false and return.
     */
    pthread_cond_broadcast(&b->ppx_to_discord.not_empty);

    /*
     * Join threads in dependency order:
     *   lws_service_thread first (uses lws_ctx)
     *   udp_rx_thread (uses lws_ctx via lws_cancel_service)
     *   udp_tx_thread (independent)
     * Then destroy the lws context once no threads are touching it.
     */
    pthread_join(b->lws_service_thread, NULL);
    pthread_join(b->udp_rx_thread,      NULL);
    pthread_join(b->udp_tx_thread,      NULL);

    if (b->lws_ctx) {
        lws_context_destroy(b->lws_ctx);
        b->lws_ctx = NULL;
        b->lws_wsi = NULL;
    }

    printf("[ppx_bridge] stopped\n");
}

void
ppx_bridge_destroy(struct ppx_bridge *b)
{
    if (!b) return;
    queue_destroy(&b->discord_to_ppx);
    queue_destroy(&b->ppx_to_discord);
    free(b);
}