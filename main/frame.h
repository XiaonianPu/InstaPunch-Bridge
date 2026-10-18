#pragma once

// Framing for the SPP <-> BLE relay (docs/protocol.md §2-§4, §6).
// Plain C with no ESP-IDF dependency so it builds and tests on a host:
// see test/test_frame.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAME_HDR_LEN 4
#define FRAME_AGE_LEN 4

#define FRAME_GAP 0x01
#define FRAME_REPLAY 0x02
#define FRAME_AGE 0x04
#define FRAME_PARTIAL 0x08

#define UP_HDR_LEN 2
#define UP_MORE 0x01

#define FRAME_FLUSH_MS 15
#define FRAME_LINE_TIMEOUT_MS 200
#define FRAME_HOLD_MAX 182 // frames a hold slot may need to resend as-is

#define DOWN_CAP 8192
#define DOWN_MARKS 256
#define WINDOW_CAP 2048
#define UP_QUEUE_CAP 4096
#define UP_RECORD_MAX 1100

typedef enum { FRAME_MODE_LINE = 0, FRAME_MODE_RAW = 1 } frame_mode_t;

// ---- Downlink: device byte stream -> frames ------------------------------

// Arrival time of the bytes ending at `end` (absolute stream position);
// `line` when the byte before `end` is '\n'.
typedef struct {
    uint32_t end;
    uint32_t t;
    bool line;
} down_mark_t;

typedef struct {
    uint8_t channel;
    frame_mode_t mode;
    uint8_t epoch;
    uint8_t seq;
    bool gap;       // set GAP on the next frame
    uint32_t lost;  // bytes dropped, total
    uint32_t in;    // absolute position after the newest byte
    uint32_t out;   // absolute position of the oldest unsent byte
    uint32_t replay_end; // bytes before this arrived while the phone was away
    uint16_t mark_head;
    uint16_t mark_count;
    down_mark_t marks[DOWN_MARKS];
    uint8_t buf[DOWN_CAP];
} down_stream_t;

void down_init(down_stream_t *s, uint8_t channel);

// A new SPP session: epoch + 1, seq restarts at 0.
void down_link_up(down_stream_t *s);

// The SPP session ended. In line mode an unterminated last line is dropped,
// since the next session cannot complete it.
void down_link_down(down_stream_t *s);

// Appends device bytes. `offline` marks them as received while no phone was
// listening (REPLAY). When full, the oldest bytes are dropped (GAP).
void down_push(down_stream_t *s, const uint8_t *data, size_t len, uint32_t now_ms, bool offline);

// Drops everything unsent without counting it as lost (nobody was listening).
void down_discard(down_stream_t *s);

size_t down_pending(const down_stream_t *s);

// Builds the next frame into `out` (at least `max_frame` bytes) and returns its
// length, or 0 when nothing should be sent yet. `force` skips the batching and
// line waits.
size_t down_pack(down_stream_t *s, uint32_t now_ms, size_t max_frame, bool force, uint8_t *out);

// ---- Ack window (hold slots) ---------------------------------------------

typedef struct {
    uint16_t used;
    uint16_t resend_pos; // next entry to resend, byte offset into buf
    bool resending;
    uint8_t buf[WINDOW_CAP]; // entries: [len][frame bytes]
} ack_window_t;

void window_init(ack_window_t *w);

// Keeps a sent frame until acked. Returns the payload bytes of older frames
// evicted to make room (counted as lost by the caller).
size_t window_push(ack_window_t *w, const uint8_t *frame, size_t len);

// Releases every frame up to and including (epoch, seq). Unknown ids are ignored.
void window_ack(ack_window_t *w, uint8_t epoch, uint8_t seq);

// Resend every unacked frame, oldest first, before any new frame.
void window_start_resend(ack_window_t *w);

// Next frame to resend, or NULL when done.
const uint8_t *window_next_resend(ack_window_t *w, size_t *len);

void window_clear(ack_window_t *w);

// ---- Uplink: write fragments -> whole records -----------------------------

typedef struct {
    uint16_t rec_len;
    bool rec_overflow; // discarding the rest of an oversized record
    uint16_t q_used;
    uint32_t lost;
    uint8_t rec[UP_RECORD_MAX];
    uint8_t q[UP_QUEUE_CAP]; // entries: [len lo][len hi][bytes]
} up_stream_t;

void up_init(up_stream_t *u);

// Adds one fragment's payload. The record is queued when `more` is false;
// records that do not fit are dropped whole.
void up_write(up_stream_t *u, const uint8_t *data, size_t len, bool more);

// Oldest queued record, or NULL.
const uint8_t *up_peek(const up_stream_t *u, size_t *len);
void up_pop(up_stream_t *u);

// Drops every queued record and any partial one, counting them as lost.
void up_clear(up_stream_t *u);

// ---- Parsing (reference for apps and tests) -------------------------------

typedef struct {
    uint8_t channel;
    uint8_t flags;
    uint8_t epoch;
    uint8_t seq;
    uint32_t age_ms;
    const uint8_t *payload;
    size_t payload_len;
} frame_view_t;

bool frame_parse(const uint8_t *frame, size_t len, frame_view_t *view);
