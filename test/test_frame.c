// Host tests for main/frame.c. Run: make -C test
// Vectors use real TruPulse 360B output captured on 2026-10-17.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

static const char *TP1 = "$PLTIT,HV,0.50,Y,240.40,D,2.90,D,0.50,Y*6E\r\n";
static const char *TP2 = "$PLTIT,HV,0.60,Y,240.40,D,1.10,D,0.60,Y*65\r\n";

static down_stream_t s_down;
static ack_window_t s_win;
static up_stream_t s_up;
static uint8_t s_frame[600];

static void push_str(down_stream_t *s, const char *text, uint32_t now, bool offline)
{
    down_push(s, (const uint8_t *)text, strlen(text), now, offline);
}

static void test_line_matches_protocol_example(void)
{
    static const uint8_t expected[] = {
        0x01, 0x00, 0x03, 0x00, 0x24, 0x50, 0x4C, 0x54, 0x49, 0x54, 0x2C, 0x48, 0x56, 0x2C, 0x30, 0x2E,
        0x35, 0x30, 0x2C, 0x59, 0x2C, 0x32, 0x34, 0x30, 0x2E, 0x34, 0x30, 0x2C, 0x44, 0x2C, 0x32, 0x2E,
        0x39, 0x30, 0x2C, 0x44, 0x2C, 0x30, 0x2E, 0x35, 0x30, 0x2C, 0x59, 0x2A, 0x36, 0x45, 0x0D, 0x0A,
    };
    down_init(&s_down, 1);
    s_down.epoch = 2;
    down_link_up(&s_down); // epoch 3
    push_str(&s_down, TP1, 1000, false);

    CHECK(down_pack(&s_down, 1005, 182, false, s_frame) == 0); // still batching
    size_t n = down_pack(&s_down, 1000 + FRAME_FLUSH_MS, 182, false, s_frame);
    CHECK(n == sizeof(expected));
    CHECK(memcmp(s_frame, expected, sizeof(expected)) == 0);
    CHECK(down_pending(&s_down) == 0);
}

static void test_fragmented_lines_share_a_frame(void)
{
    down_init(&s_down, 1);
    down_link_up(&s_down);
    // One measurement split across SPP callbacks, then a second one.
    down_push(&s_down, (const uint8_t *)TP1, 10, 0, false);
    down_push(&s_down, (const uint8_t *)TP1 + 10, strlen(TP1) - 10, 2, false);
    push_str(&s_down, TP2, 4, false);

    size_t n = down_pack(&s_down, 20, 182, false, s_frame);
    frame_view_t v;
    CHECK(frame_parse(s_frame, n, &v));
    CHECK(v.flags == 0);
    CHECK(v.payload_len == strlen(TP1) + strlen(TP2));
    CHECK(memcmp(v.payload, TP1, strlen(TP1)) == 0);
    CHECK(memcmp(v.payload + strlen(TP1), TP2, strlen(TP2)) == 0);
}

static void test_only_whole_lines_fill_a_frame(void)
{
    down_init(&s_down, 2);
    push_str(&s_down, TP1, 0, false);
    push_str(&s_down, TP2, 0, false);
    // Room for one and a half lines: send exactly the first line.
    size_t max_frame = FRAME_HDR_LEN + strlen(TP1) + 20;
    size_t n = down_pack(&s_down, 100, max_frame, false, s_frame);
    CHECK(n == FRAME_HDR_LEN + strlen(TP1));
    CHECK(s_frame[1] == 0);
    n = down_pack(&s_down, 100, max_frame, false, s_frame);
    CHECK(n == FRAME_HDR_LEN + strlen(TP2));
    CHECK(s_frame[3] == 1); // seq
}

static void test_long_line_splits_with_partial(void)
{
    down_init(&s_down, 1);
    push_str(&s_down, TP1, 0, false); // 44 bytes, MTU 23 -> 19-byte frames, 15 bytes payload
    size_t total = 0;
    int frames = 0;
    uint8_t last_flags = 0;
    size_t n;
    while ((n = down_pack(&s_down, 100, 20, false, s_frame)) > 0) {
        frame_view_t v;
        CHECK(frame_parse(s_frame, n, &v));
        CHECK(memcmp(v.payload, TP1 + total, v.payload_len) == 0);
        total += v.payload_len;
        last_flags = v.flags;
        if (total < strlen(TP1)) {
            CHECK(v.flags & FRAME_PARTIAL);
        }
        frames++;
    }
    CHECK(total == strlen(TP1));
    CHECK(frames == 3);
    CHECK(!(last_flags & FRAME_PARTIAL));
}

static void test_unterminated_line_waits_then_partial(void)
{
    down_init(&s_down, 1);
    push_str(&s_down, "$PLTIT,HV,0.50", 0, false);
    CHECK(down_pack(&s_down, 50, 182, false, s_frame) == 0);
    CHECK(down_pack(&s_down, FRAME_LINE_TIMEOUT_MS - 1, 182, false, s_frame) == 0);
    size_t n = down_pack(&s_down, FRAME_LINE_TIMEOUT_MS, 182, false, s_frame);
    CHECK(n == FRAME_HDR_LEN + 14);
    CHECK(s_frame[1] == FRAME_PARTIAL);
}

static void test_raw_mode_ignores_newlines(void)
{
    static const uint8_t rtcm[] = {0xD3, 0x00, 0x13, 0x0A, 0x3E, 0xD0, 0x0D, 0x0A, 0x11};
    down_init(&s_down, 2);
    s_down.mode = FRAME_MODE_RAW;
    down_push(&s_down, rtcm, sizeof(rtcm), 0, false);
    CHECK(down_pack(&s_down, 5, 182, false, s_frame) == 0);
    size_t n = down_pack(&s_down, FRAME_FLUSH_MS, 182, false, s_frame);
    CHECK(n == FRAME_HDR_LEN + sizeof(rtcm));
    CHECK(s_frame[1] == 0);
    CHECK(memcmp(s_frame + FRAME_HDR_LEN, rtcm, sizeof(rtcm)) == 0);
}

static void test_overflow_sets_gap_and_counts(void)
{
    down_init(&s_down, 1);
    s_down.mode = FRAME_MODE_RAW;
    static uint8_t big[DOWN_CAP];
    memset(big, 'A', sizeof(big));
    down_push(&s_down, big, sizeof(big), 0, false);
    down_push(&s_down, (const uint8_t *)"BBBB", 4, 0, false);
    CHECK(s_down.lost == 4);
    CHECK(down_pending(&s_down) == DOWN_CAP);
    size_t n = down_pack(&s_down, 100, 182, false, s_frame);
    CHECK(n == 182);
    CHECK(s_frame[1] & FRAME_GAP);
    n = down_pack(&s_down, 100, 182, false, s_frame);
    CHECK(!(s_frame[1] & FRAME_GAP));

    // A single push larger than the buffer keeps only its newest bytes.
    down_init(&s_down, 1);
    s_down.mode = FRAME_MODE_RAW;
    static uint8_t huge[DOWN_CAP + 100];
    memset(huge, 'C', sizeof(huge));
    huge[sizeof(huge) - 1] = 'Z';
    down_push(&s_down, huge, sizeof(huge), 0, false);
    CHECK(s_down.lost == 100);
    CHECK(down_pending(&s_down) == DOWN_CAP);
}

static void test_offline_bytes_replay_with_age(void)
{
    down_init(&s_down, 1);
    push_str(&s_down, TP1, 1000, true); // phone away
    push_str(&s_down, TP2, 5000, false); // phone back
    size_t n = down_pack(&s_down, 6000, 182, false, s_frame);
    frame_view_t v;
    CHECK(frame_parse(s_frame, n, &v));
    CHECK(v.flags == (FRAME_REPLAY | FRAME_AGE));
    CHECK(v.age_ms == 5000);
    CHECK(v.payload_len == strlen(TP1)); // replay frames stop at the live boundary
    n = down_pack(&s_down, 6000, 182, false, s_frame);
    CHECK(frame_parse(s_frame, n, &v));
    CHECK(v.flags == 0);
    CHECK(v.payload_len == strlen(TP2));
}

static void test_link_down_drops_unterminated_line(void)
{
    down_init(&s_down, 1);
    push_str(&s_down, TP1, 0, false);
    push_str(&s_down, "$PLTIT,HV,0.", 0, false);
    down_link_down(&s_down);
    CHECK(s_down.lost == 12);
    CHECK(down_pending(&s_down) == strlen(TP1));
    down_link_up(&s_down);
    size_t n = down_pack(&s_down, 100, 182, false, s_frame);
    CHECK(n == FRAME_HDR_LEN + strlen(TP1));
    CHECK(s_frame[1] == FRAME_GAP);
    CHECK(s_frame[2] == 1);
}

static void test_discard_is_not_lost(void)
{
    down_init(&s_down, 1);
    push_str(&s_down, TP1, 0, false);
    down_discard(&s_down);
    CHECK(s_down.lost == 0);
    CHECK(down_pending(&s_down) == 0);
    push_str(&s_down, TP2, 10, false);
    size_t n = down_pack(&s_down, 100, 182, false, s_frame);
    CHECK(n > 0 && (s_frame[1] & FRAME_GAP));
}

static size_t make_frame(uint8_t epoch, uint8_t seq, uint8_t *out)
{
    out[0] = 1;
    out[1] = 0;
    out[2] = epoch;
    out[3] = seq;
    memcpy(out + FRAME_HDR_LEN, TP1, strlen(TP1));
    return FRAME_HDR_LEN + strlen(TP1);
}

static void test_window_ack_and_resend(void)
{
    uint8_t f[100];
    window_init(&s_win);
    for (uint8_t seq = 0; seq < 3; seq++) {
        size_t len = make_frame(7, seq, f);
        CHECK(window_push(&s_win, f, len) == 0);
    }
    window_ack(&s_win, 7, 1); // releases seq 0 and 1
    window_ack(&s_win, 9, 9); // unknown: ignored
    window_start_resend(&s_win);
    size_t len;
    const uint8_t *frame = window_next_resend(&s_win, &len);
    CHECK(frame && frame[3] == 2 && len == FRAME_HDR_LEN + strlen(TP1));
    CHECK(window_next_resend(&s_win, &len) == NULL);
    CHECK(!s_win.resending);

    window_ack(&s_win, 7, 2);
    CHECK(s_win.used == 0);
}

static void test_window_evicts_oldest_when_full(void)
{
    uint8_t f[100];
    window_init(&s_win);
    size_t entry = 1 + make_frame(0, 0, f);
    size_t fits = WINDOW_CAP / entry;
    size_t evicted = 0;
    for (size_t i = 0; i <= fits; i++) {
        size_t len = make_frame(0, (uint8_t)i, f);
        evicted += window_push(&s_win, f, len);
    }
    CHECK(evicted == strlen(TP1));
    window_start_resend(&s_win);
    size_t len;
    const uint8_t *frame = window_next_resend(&s_win, &len);
    CHECK(frame && frame[3] == 1);
}

static void test_uplink_records(void)
{
    up_init(&s_up);
    static const uint8_t part1[] = {0xD3, 0x00, 0x04};
    static const uint8_t part2[] = {0x10, 0x20, 0x30, 0x40};
    up_write(&s_up, part1, sizeof(part1), true);
    size_t len;
    CHECK(up_peek(&s_up, &len) == NULL); // not complete yet
    up_write(&s_up, part2, sizeof(part2), false);
    const uint8_t *rec = up_peek(&s_up, &len);
    CHECK(rec && len == 7 && rec[0] == 0xD3 && rec[6] == 0x40);
    up_pop(&s_up);
    CHECK(up_peek(&s_up, &len) == NULL);

    // An oversized record is dropped whole, including its later fragments.
    static uint8_t chunk[600];
    up_write(&s_up, chunk, sizeof(chunk), true);
    up_write(&s_up, chunk, sizeof(chunk), true);
    up_write(&s_up, part2, sizeof(part2), false);
    CHECK(up_peek(&s_up, &len) == NULL);
    CHECK(s_up.lost == 1200 + sizeof(part2));

    // A full queue drops the new record whole.
    up_init(&s_up);
    static uint8_t rec_1k[1000];
    for (int i = 0; i < 5; i++) {
        up_write(&s_up, rec_1k, sizeof(rec_1k), false);
    }
    CHECK(s_up.q_used == 4 * (2 + 1000));
    CHECK(s_up.lost == 1000);
    up_clear(&s_up);
    CHECK(s_up.lost == 5000);
    CHECK(up_peek(&s_up, &len) == NULL);
}

static void test_stream_positions_wrap(void)
{
    down_init(&s_down, 1);
    s_down.in = s_down.out = s_down.replay_end = 0xFFFFFFF0u;
    push_str(&s_down, TP1, 0, false);
    push_str(&s_down, TP2, 0, false);
    size_t n = down_pack(&s_down, 100, 182, false, s_frame);
    CHECK(n == FRAME_HDR_LEN + strlen(TP1) + strlen(TP2));
    CHECK(memcmp(s_frame + FRAME_HDR_LEN, TP1, strlen(TP1)) == 0);
}

int main(void)
{
    test_line_matches_protocol_example();
    test_fragmented_lines_share_a_frame();
    test_only_whole_lines_fill_a_frame();
    test_long_line_splits_with_partial();
    test_unterminated_line_waits_then_partial();
    test_raw_mode_ignores_newlines();
    test_overflow_sets_gap_and_counts();
    test_offline_bytes_replay_with_age();
    test_link_down_drops_unterminated_line();
    test_discard_is_not_lost();
    test_window_ack_and_resend();
    test_window_evicts_oldest_when_full();
    test_uplink_records();
    test_stream_positions_wrap();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    printf("frame tests passed\n");
    return EXIT_SUCCESS;
}
