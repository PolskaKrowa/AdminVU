/**
 * @file personaplex_bridge.h
 * @brief Discord Voice UDP <-> NVIDIA PersonaPlex WebSocket audio bridge.
 *
 * This module sits between orca's voice connection and a running PersonaPlex
 * server. It handles everything between them:
 *
 *   Discord UDP  -->  decrypt (libsodium)  -->  strip RTP header
 *       -->  prepend 0x01 type byte  -->  WebSocket send to PersonaPlex
 *
 *   PersonaPlex  -->  WebSocket recv  -->  strip 0x01 type byte
 *       -->  build RTP header  -->  encrypt (libsodium)  -->  Discord UDP
 *
 * No Opus encoding or decoding happens here. Both Discord and PersonaPlex
 * speak Opus natively, so the module is purely a crypto+framing pipe.
 *
 * ----------------------------------------------------------------------------
 * Dependencies
 * ----------------------------------------------------------------------------
 *   libsodium     -- Discord voice encryption (xsalsa20_poly1305)
 *   libwebsockets -- WebSocket client for PersonaPlex
 *
 * Link with: -lsodium -lwebsockets -lpthread
 *
 * ----------------------------------------------------------------------------
 * Usage
 * ----------------------------------------------------------------------------
 * Call this after Discord fires on_session_description (which delivers the
 * secret key) and after you have sent SELECT PROTOCOL (op 1) to Discord:
 *
 * @code
 *   // Called from your on_session_description callback:
 *   static void on_session_desc(struct discord_voice *vc) {
 *       // Parse secret_key and server info from vc->payload.event_data
 *       // (see discord-voice-connections.c for context)
 *
 *       struct ppx_bridge_config cfg = {
 *           .discord_server_port       = vc->udp_service.server_port,
 *           .discord_ssrc              = vc->udp_service.ssrc,
 *           .personaplex_port          = 8998,
 *           .personaplex_use_tls       = true,
 *           .personaplex_skip_cert_verify = true,  // for dev self-signed certs
 *       };
 *       strncpy(cfg.discord_server_ip,  vc->udp_service.server_ip,
 *               sizeof(cfg.discord_server_ip) - 1);
 *       strncpy(cfg.personaplex_host, "localhost",
 *               sizeof(cfg.personaplex_host) - 1);
 *       strncpy(cfg.personaplex_path, "/api/chat",
 *               sizeof(cfg.personaplex_path) - 1);
 *       memcpy(cfg.discord_secret_key, your_secret_key, PPX_SECRET_KEY_LEN);
 *
 *       g_bridge = ppx_bridge_create(&cfg);
 *       ppx_bridge_start(g_bridge);
 *   }
 *
 *   // When the bot leaves or the session ends:
 *   ppx_bridge_stop(g_bridge);
 *   ppx_bridge_destroy(g_bridge);
 * @endcode
 *
 * ----------------------------------------------------------------------------
 * Threading model
 * ----------------------------------------------------------------------------
 * The bridge spawns three internal threads:
 *
 *   udp_rx_thread     -- blocks on recvfrom(), decrypts Discord packets,
 *                        strips RTP headers, pushes raw Opus into a queue,
 *                        then wakes the WebSocket service loop.
 *
 *   lws_service_thread-- runs the libwebsockets event loop; on WRITEABLE
 *                        drains the udp->ppx queue and sends audio frames;
 *                        on RECEIVE pushes PersonaPlex Opus into the ppx->udp
 *                        queue.
 *
 *   udp_tx_thread     -- blocks on the ppx->udp queue, builds RTP headers,
 *                        encrypts with libsodium, and sends to Discord.
 *
 * ----------------------------------------------------------------------------
 * Sample-rate note
 * ----------------------------------------------------------------------------
 * Discord sends Opus at 48 kHz stereo (960 samples / 20 ms frame).
 * PersonaPlex / Moshi operates at 24 kHz (1920 samples / ~80 ms internally).
 * When PersonaPlex's Python server decodes incoming Opus it can target any
 * sample rate, so 48 kHz frames are generally accepted without error. If you
 * hear pitch or speed artefacts, you will need to decode -> resample -> re-
 * encode before bridging (which requires libopus in your C code). For most
 * conversational use cases this is not necessary.
 */

#ifndef PERSONAPLEX_BRIDGE_H
#define PERSONAPLEX_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Constants you may want to tune
 * ------------------------------------------------------------------------- */

/**
 * Maximum Opus frame payload Discord will ever send.
 * Discord caps at 1276 bytes (RFC 6716 limit for one 120 ms Opus frame).
 */
#define PPX_MAX_OPUS_FRAME 1276

/** Length of the Discord shared secret key (from SESSION_DESCRIPTION). */
#define PPX_SECRET_KEY_LEN 32

/**
 * Number of Opus frames held in each internal ring buffer.
 * 32 frames * 20 ms = ~640 ms of headroom before frames are dropped.
 * Increase if you see "queue full" warnings; decrease to reduce latency.
 */
#define PPX_QUEUE_CAPACITY 32

/* -------------------------------------------------------------------------
 * PersonaPlex binary WebSocket protocol bytes
 * ------------------------------------------------------------------------- */

/** First byte of a PersonaPlex WebSocket message: initial handshake. */
#define PPX_MSG_HANDSHAKE 0x00

/** First byte of a PersonaPlex WebSocket message: Opus audio frame. */
#define PPX_MSG_AUDIO     0x01

/** First byte of a PersonaPlex WebSocket message: UTF-8 text/prompt. */
#define PPX_MSG_TEXT      0x02

/** Protocol version sent in the handshake. */
#define PPX_PROTOCOL_VERSION 0x00

/** Model identifier sent in the handshake (0 = default). */
#define PPX_PROTOCOL_MODEL   0x00

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

/**
 * @brief All configuration needed to start a bridge session.
 *
 * Populate this after Discord's SESSION_DESCRIPTION event fires
 * and before calling ppx_bridge_start().
 */
struct ppx_bridge_config {

    /* ------------------------------------------------------------------
     * Discord voice UDP side
     * ------------------------------------------------------------------ */

    /**
     * IPv4 address of Discord's voice UDP server.
     * Comes from the READY payload (opcode 2) field "ip".
     * Example: "185.60.112.157"
     */
    char discord_server_ip[64];

    /**
     * UDP port of Discord's voice server.
     * Comes from the READY payload field "port".
     */
    uint16_t discord_server_port;

    /**
     * Our SSRC assigned by Discord.
     * Comes from the READY payload field "ssrc".
     * Used in outgoing RTP headers and in the IP discovery request.
     */
    uint32_t discord_ssrc;

    /**
     * 32-byte shared encryption key from Discord's SESSION_DESCRIPTION
     * (opcode 4) field "secret_key".
     *
     * This is used with xsalsa20_poly1305 to encrypt every UDP packet
     * we send and decrypt every UDP packet we receive.
     *
     * Never log or expose this value.
     */
    uint8_t discord_secret_key[PPX_SECRET_KEY_LEN];

    /* ------------------------------------------------------------------
     * PersonaPlex WebSocket side
     * ------------------------------------------------------------------ */

    /**
     * Hostname or IP address of the PersonaPlex server.
     * Usually "localhost" when running locally, or the server IP for
     * a remote deployment.
     */
    char personaplex_host[256];

    /**
     * Port PersonaPlex is listening on.
     * Default is 8998.
     */
    uint16_t personaplex_port;

    /**
     * WebSocket path for the chat endpoint.
     * Default is "/api/chat".
     */
    char personaplex_path[256];

    /**
     * Set to true to connect over wss:// (TLS).
     * PersonaPlex always starts with TLS, so this should almost always
     * be true.
     */
    bool personaplex_use_tls;

    /**
     * Set to true to accept self-signed TLS certificates.
     * Enable this when PersonaPlex is using its auto-generated dev certs
     * (the default when launched with --ssl <tmpdir>).
     */
    bool personaplex_skip_cert_verify;
};

/* -------------------------------------------------------------------------
 * Opaque bridge handle
 * ------------------------------------------------------------------------- */

/** Internal state. Do not access members directly. */
struct ppx_bridge;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate and initialise a new bridge.
 *
 * Does not open sockets or start threads. Call ppx_bridge_start() for that.
 *
 * @param cfg  Fully populated configuration. Copied internally; safe to
 *             stack-allocate and discard after this call.
 * @return     A new bridge handle, or NULL if allocation fails.
 */
struct ppx_bridge *ppx_bridge_create(const struct ppx_bridge_config *cfg);

/**
 * @brief Start the bridge.
 *
 * In order:
 *   1. Opens the UDP socket and binds it.
 *   2. Performs Discord UDP IP discovery to establish the UDP path.
 *   3. Connects to PersonaPlex over WebSocket and sends the handshake.
 *   4. Starts the three internal threads.
 *
 * After this returns 0, Opus audio flows automatically in both directions.
 *
 * @param bridge  Handle from ppx_bridge_create().
 * @return        0 on success, -1 on error (details printed to stderr).
 */
int ppx_bridge_start(struct ppx_bridge *bridge);

/**
 * @brief Stop the bridge and shut down all connections.
 *
 * Sets the stop flag, closes the UDP socket to unblock the receive thread,
 * then joins all three threads. Blocks until everything has cleanly exited.
 *
 * Safe to call from any thread, including an orca voice callback.
 *
 * @param bridge  Handle from ppx_bridge_create().
 */
void ppx_bridge_stop(struct ppx_bridge *bridge);

/**
 * @brief Free the bridge and all its resources.
 *
 * Must only be called after ppx_bridge_stop(). Calling this on a running
 * bridge is undefined behaviour.
 *
 * @param bridge  Handle from ppx_bridge_create().
 */
void ppx_bridge_destroy(struct ppx_bridge *bridge);

#endif /* PERSONAPLEX_BRIDGE_H */