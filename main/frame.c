#include "frame.h"

#include <string.h>

_Static_assert((DOWN_CAP & (DOWN_CAP - 1)) == 0, "DOWN_CAP must be a power of two");
_Static_assert(FRAME_HOLD_MAX <= 255, "window entries store a one-byte length");

// Stream positions are absolute uint32 counters; compare them with wraparound.
static bool before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

// ---- Downlink ----------------------------------------------------------

static down_mark_t *mark_at(down_stream_t *s, uint16_t i)
{
    return &s->marks[(s->mark_head + i) % DOWN_MARKS];
}

static void marks_pop_front(down_stream_t *s)
{
    s->mark_head = (s->mark_head + 1) % DOWN_MARKS;
    s->mark_count--;
}

static void marks_trim_front(down_stream_t *s)
{
    while (s->mark_count > 0 && !before(s->out, mark_at(s, 0)->end)) {
        marks_pop_front(s);
    }
}

static void marks_add(down_stream_t *s, uint32_t end, uint32_t t, bool line)
{
    if (s->mark_count == DOWN_MARKS) {
        marks_pop_front(s); // oldest bytes lose their timestamp, never data
    }
    down_mark_t *m = mark_at(s, s->mark_count++);
    m->end = end;
    m->t = t;
    m->line = line;
}

// Arrival time of the byte at `pos`.
static uint32_t time_at(down_stream_t *s, uint32_t pos, uint32_t now)
{
    for (uint16_t i = 0; i < s->mark_count; i++) {
        down_mark_t *m = mark_at(s, i);
        if (before(pos, m->end)) {
            return m->t;
        }
    }
    return now;
}

// Last line end in (out, limit], or `out` when there is none.
static uint32_t last_line_end(down_stream_t *s, uint32_t limit)
{
    uint32_t found = s->out;
    for (uint16_t i = 0; i < s->mark_count; i++) {
        down_mark_t *m = mark_at(s, i);
        if (before(limit, m->end)) {
            break;
        }
        if (m->line && before(s->out, m->end)) {
            found = m->end;
        }
    }
    return found;
}

static void ring_copy_in(down_stream_t *s, uint32_t pos, const uint8_t *data, size_t len)
{
    size_t idx = pos & (DOWN_CAP - 1);
    size_t first = min_size(len, DOWN_CAP - idx);
    memcpy(s->buf + idx, data, first);
    memcpy(s->buf, data + first, len - first);
}

static void ring_copy_out(const down_stream_t *s, uint32_t pos, uint8_t *dst, size_t len)
{
    size_t idx = pos & (DOWN_CAP - 1);
    size_t first = min_size(len, DOWN_CAP - idx);
    memcpy(dst, s->buf + idx, first);
    memcpy(dst + first, s->buf, len - first);
}

static uint8_t ring_byte(const down_stream_t *s, uint32_t pos)
{
    return s->buf[pos & (DOWN_CAP - 1)];
}

void down_init(down_stream_t *s, uint8_t channel)
{
    memset(s, 0, sizeof(*s));
    s->channel = channel;
}

void down_link_up(down_stream_t *s)
{
    s->epoch++;
    s->seq = 0;
}

void down_link_down(down_stream_t *s)
{
    if (s->mode != FRAME_MODE_LINE || s->in == s->out) {
        return;
    }
    uint32_t keep = last_line_end(s, s->in);
    if (keep == s->in) {
        return;
    }
    s->lost += s->in - keep;
    s->in = keep;
    s->gap = true;
    while (s->mark_count > 0 && before(keep, mark_at(s, s->mark_count - 1)->end)) {
        s->mark_count--;
    }
    if (before(keep, s->replay_end)) {
        s->replay_end = keep;
    }
}

void down_push(down_stream_t *s, const uint8_t *data, size_t len, uint32_t now_ms, bool offline)
{
    if (len == 0) {
        return;
    }
    if (len > DOWN_CAP) {
        // Keep only the newest DOWN_CAP bytes; skipped bytes keep their stream positions.
        size_t skip = len - DOWN_CAP;
        s->lost += (uint32_t)(s->in - s->out) + (uint32_t)skip;
        s->in += (uint32_t)skip;
        s->out = s->in;
        s->mark_count = 0;
        s->gap = true;
        data += skip;
        len = DOWN_CAP;
    }
    size_t free_space = DOWN_CAP - (s->in - s->out);
    if (len > free_space) {
        size_t drop = len - free_space;
        s->out += (uint32_t)drop;
        s->lost += (uint32_t)drop;
        s->gap = true;
        marks_trim_front(s);
    }

    ring_copy_in(s, s->in, data, len);
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    while (p < end) {
        const uint8_t *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) {
            marks_add(s, s->in + (uint32_t)len, now_ms, false);
            break;
        }
        marks_add(s, s->in + (uint32_t)(nl + 1 - data), now_ms, true);
        p = nl + 1;
    }
    s->in += (uint32_t)len;
    if (offline) {
        s->replay_end = s->in;
    }
}

void down_discard(down_stream_t *s)
{
    if (s->in != s->out) {
        s->gap = true;
    }
    s->out = s->in;
    s->replay_end = s->in;
    s->mark_count = 0;
}

size_t down_pending(const down_stream_t *s)
{
    return s->in - s->out;
}

size_t down_pack(down_stream_t *s, uint32_t now_ms, size_t max_frame, bool force, uint8_t *out)
{
    size_t avail = s->in - s->out;
    if (avail == 0) {
        return 0;
    }
    bool replay = before(s->out, s->replay_end);
    size_t hdr = FRAME_HDR_LEN + (replay ? FRAME_AGE_LEN : 0);
    if (max_frame <= hdr) {
        return 0;
    }
    size_t room = max_frame - hdr;
    size_t limit = replay ? min_size(avail, s->replay_end - s->out) : avail;
    uint32_t first_t = time_at(s, s->out, now_ms);
    uint32_t age = now_ms - first_t;
    bool urgent = force || replay;

    // Give a burst (several NMEA lines, a fragmented record) a moment to fill the frame.
    if (!urgent && limit < room && age < FRAME_FLUSH_MS) {
        return 0;
    }

    size_t take;
    if (s->mode == FRAME_MODE_RAW) {
        take = min_size(limit, room);
    } else {
        uint32_t line_end = last_line_end(s, s->out + (uint32_t)min_size(limit, room));
        if (line_end != s->out) {
            take = line_end - s->out;
        } else if (limit > room) {
            take = room; // a single line longer than the frame
        } else if (urgent || age >= FRAME_LINE_TIMEOUT_MS) {
            take = limit; // unterminated line, waited long enough
        } else {
            return 0;
        }
    }

    bool partial = s->mode == FRAME_MODE_LINE && ring_byte(s, s->out + (uint32_t)take - 1) != '\n';
    uint8_t flags = (s->gap ? FRAME_GAP : 0) | (replay ? FRAME_REPLAY | FRAME_AGE : 0) |
                    (partial ? FRAME_PARTIAL : 0);
    out[0] = s->channel;
    out[1] = flags;
    out[2] = s->epoch;
    out[3] = s->seq++;
    if (replay) {
        out[4] = (uint8_t)age;
        out[5] = (uint8_t)(age >> 8);
        out[6] = (uint8_t)(age >> 16);
        out[7] = (uint8_t)(age >> 24);
    }
    ring_copy_out(s, s->out, out + hdr, take);
    s->out += (uint32_t)take;
    s->gap = false;
    marks_trim_front(s);
    return hdr + take;
}

// ---- Ack window --------------------------------------------------------

static size_t frame_payload_len(const uint8_t *frame, size_t len)
{
    size_t hdr = FRAME_HDR_LEN + ((frame[1] & FRAME_AGE) ? FRAME_AGE_LEN : 0);
    return len > hdr ? len - hdr : 0;
}

static void window_release(ack_window_t *w, size_t n)
{
    memmove(w->buf, w->buf + n, w->used - n);
    w->used -= (uint16_t)n;
    w->resend_pos = w->resend_pos > n ? (uint16_t)(w->resend_pos - n) : 0;
}

void window_init(ack_window_t *w)
{
    memset(w, 0, sizeof(*w));
}

size_t window_push(ack_window_t *w, const uint8_t *frame, size_t len)
{
    size_t evicted = 0;
    if (len > FRAME_HOLD_MAX) {
        return 0;
    }
    while (w->used + 1 + len > WINDOW_CAP && w->used > 0) {
        size_t old_len = w->buf[0];
        evicted += frame_payload_len(w->buf + 1, old_len);
        window_release(w, 1 + old_len);
    }
    w->buf[w->used] = (uint8_t)len;
    memcpy(w->buf + w->used + 1, frame, len);
    w->used += (uint16_t)(1 + len);
    return evicted;
}

void window_ack(ack_window_t *w, uint8_t epoch, uint8_t seq)
{
    size_t pos = 0;
    while (pos < w->used) {
        size_t len = w->buf[pos];
        const uint8_t *frame = w->buf + pos + 1;
        pos += 1 + len;
        if (frame[2] == epoch && frame[3] == seq) {
            window_release(w, pos);
            return;
        }
    }
}

void window_start_resend(ack_window_t *w)
{
    w->resend_pos = 0;
    w->resending = w->used > 0;
}

const uint8_t *window_next_resend(ack_window_t *w, size_t *len)
{
    if (!w->resending || w->resend_pos >= w->used) {
        w->resending = false;
        return NULL;
    }
    *len = w->buf[w->resend_pos];
    const uint8_t *frame = w->buf + w->resend_pos + 1;
    w->resend_pos += (uint16_t)(1 + *len);
    return frame;
}

void window_clear(ack_window_t *w)
{
    w->used = 0;
    w->resend_pos = 0;
    w->resending = false;
}

// ---- Uplink ------------------------------------------------------------

void up_init(up_stream_t *u)
{
    memset(u, 0, sizeof(*u));
}

void up_write(up_stream_t *u, const uint8_t *data, size_t len, bool more)
{
    if (u->rec_overflow) {
        u->lost += (uint32_t)len;
        u->rec_overflow = more;
        return;
    }
    if (u->rec_len + len > UP_RECORD_MAX) {
        u->lost += (uint32_t)(u->rec_len + len);
        u->rec_len = 0;
        u->rec_overflow = more;
        return;
    }
    memcpy(u->rec + u->rec_len, data, len);
    u->rec_len += (uint16_t)len;
    if (more) {
        return;
    }
    if (u->rec_len == 0) {
        return;
    }
    if (u->q_used + 2 + u->rec_len > UP_QUEUE_CAP) {
        u->lost += u->rec_len;
    } else {
        u->q[u->q_used] = (uint8_t)u->rec_len;
        u->q[u->q_used + 1] = (uint8_t)(u->rec_len >> 8);
        memcpy(u->q + u->q_used + 2, u->rec, u->rec_len);
        u->q_used += (uint16_t)(2 + u->rec_len);
    }
    u->rec_len = 0;
}

const uint8_t *up_peek(const up_stream_t *u, size_t *len)
{
    if (u->q_used == 0) {
        return NULL;
    }
    *len = (size_t)u->q[0] | ((size_t)u->q[1] << 8);
    return u->q + 2;
}

void up_pop(up_stream_t *u)
{
    size_t len;
    if (!up_peek(u, &len)) {
        return;
    }
    size_t n = 2 + len;
    memmove(u->q, u->q + n, u->q_used - n);
    u->q_used -= (uint16_t)n;
}

void up_clear(up_stream_t *u)
{
    size_t pos = 0;
    while (pos < u->q_used) {
        size_t len = (size_t)u->q[pos] | ((size_t)u->q[pos + 1] << 8);
        u->lost += (uint32_t)len;
        pos += 2 + len;
    }
    u->lost += u->rec_len;
    u->q_used = 0;
    u->rec_len = 0;
    u->rec_overflow = false;
}

// ---- Parsing -----------------------------------------------------------

bool frame_parse(const uint8_t *frame, size_t len, frame_view_t *view)
{
    if (len < FRAME_HDR_LEN + 1) {
        return false;
    }
    view->channel = frame[0];
    view->flags = frame[1];
    view->epoch = frame[2];
    view->seq = frame[3];
    view->age_ms = 0;
    size_t hdr = FRAME_HDR_LEN;
    if (view->flags & FRAME_AGE) {
        if (len < FRAME_HDR_LEN + FRAME_AGE_LEN + 1) {
            return false;
        }
        view->age_ms = (uint32_t)frame[4] | ((uint32_t)frame[5] << 8) | ((uint32_t)frame[6] << 16) |
                       ((uint32_t)frame[7] << 24);
        hdr += FRAME_AGE_LEN;
    }
    view->payload = frame + hdr;
    view->payload_len = len - hdr;
    return true;
}
