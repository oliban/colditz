#if !defined(WIN32) && !defined(PSP)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include "agent_api.h"
#include "colditz.h"   /* guybrush, p_event, game_state, props, game_time */
#include "game.h"      /* guybrush[] extern */
#include "conf.h"      /* KEY_* macros (resolve via loaded colditz.ini) */
#include "low-level.h" /* readlong/readword, used by readtile/readexit macros */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

bool agent_api_enabled = false;
static int listen_fd = -1;

/* room_x/room_y/offset are the engine's own working globals (defined in
 * game.c, used by the readtile()/readexit() macros in game.h -- see
 * set_room_xy(), game.c:707). game.h does not declare them extern itself;
 * every other engine file that needs them (e.g. graphics.c:57-59) adds its
 * own extern declaration, so we follow the same pattern here. */
extern uint16_t room_x, room_y;
extern uint32_t offset;

/* Same pattern: remove_props is a game.c global (not declared extern in
 * colditz.h), used by set_props_overlays() (game.c:1001) to hide a prop
 * overlay covered by a removable outside wall. /room's items list mirrors
 * that same visibility test (see handle_room) so it only ever reports
 * props the renderer would actually draw. */
extern uint8_t remove_props[CMP_MAP_WIDTH][CMP_MAP_HEIGHT];

/* Grow-on-demand buffer used by the stb PNG-encode write callback. Declared
 * once at file scope (rather than duplicated locally in png_append and
 * handle_screen) so both share the identical definition. */
struct growbuf { uint8_t* p; size_t len, cap; };

static uint8_t* frame_buf = NULL;
static int frame_w = 0, frame_h = 0;

void agent_api_init(uint16_t port)
{
    struct sockaddr_in addr;
    int one = 1;
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("agent_api: socket"); return; }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, 16) < 0)
    {
        perror("agent_api: bind/listen (API disabled)");
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    agent_api_enabled = true;
    /* stb's default PNG compression level (8) is slow enough on a full
     * frame to cause a visible hitch on the game's own render thread when
     * a dashboard polls /screen at any real cadence. Level 1 trades file
     * size (still well within send_response's deadline) for speed -- see
     * docs/AGENT-API.md for the measured size delta. */
    stbi_write_png_compression_level = 1;
    printf("agent_api: listening on 127.0.0.1:%u\n", port);
}

/* Milliseconds elapsed since t0, per gettimeofday(). Used to bound the
 * total wall-clock time send_response() spends writing, so a client that
 * drip-reads (SO_SNDTIMEO only caps each individual write() call, not the
 * sum of many small successful ones) can't stall the GLUT loop. */
static long ms_since(const struct timeval* t0)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - t0->tv_sec) * 1000L +
           (now.tv_usec - t0->tv_usec) / 1000L;
}

#define SEND_RESPONSE_DEADLINE_MS 400

static void send_response(int cfd, int code, const char* ctype,
                          const void* body, size_t len)
{
    char hdr[256];
    struct timeval t0;
    const char* msg = (code==200)?"OK":(code==202)?"Accepted":
                      (code==400)?"Bad Request":(code==404)?"Not Found":
                      (code==500)?"Internal Server Error":"Error";
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        code, msg, ctype, len);
    if (n < 0) return;
    if (n >= (int)sizeof(hdr)) n = (int)sizeof(hdr) - 1;
    gettimeofday(&t0, NULL);
    size_t hdr_off = 0;
    while (hdr_off < (size_t)n) {
        ssize_t w;
        if (ms_since(&t0) > SEND_RESPONSE_DEADLINE_MS) break;
        w = write(cfd, hdr + hdr_off, n - hdr_off);
        if (w <= 0) break;
        hdr_off += (size_t)w;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w;
        if (ms_since(&t0) > SEND_RESPONSE_DEADLINE_MS) break;
        w = write(cfd, (const char*)body + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

/* Captures the current GL back-buffer contents into frame_buf. Called once
 * per rendered frame from glut_display(), right before glutSwapBuffers(),
 * so what's read here is still the back buffer (not yet promoted to front).
 * Costs nothing when the API is disabled. */
void agent_api_capture(void)
{
    if (!agent_api_enabled) return;
    /* Also re-allocate if a previous allocation attempt failed (frame_buf
     * is NULL) even though the dimensions haven't changed, so a transient
     * OOM doesn't permanently disable capture. */
    if (frame_w != gl_width || frame_h != gl_height || !frame_buf) {
        free(frame_buf);
        frame_buf = malloc((size_t)gl_width * (size_t)gl_height * 3);
        frame_w = gl_width; frame_h = gl_height;
    }
    if (!frame_buf) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, frame_w, frame_h, GL_RGB, GL_UNSIGNED_BYTE, frame_buf);
}

/* stb PNG-encode write callback: appends `size` bytes to the growbuf,
 * growing it geometrically as needed. If realloc() fails, the partial
 * buffer is freed and the growbuf reset to empty rather than left
 * dangling/leaked; the caller (handle_screen) treats an empty growbuf as
 * "encode failed" and responds 500 instead of crashing on a NULL body. */
static void png_append(void* ctx, void* data, int size)
{
    struct growbuf *g = ctx;
    if (size <= 0) return;
    if (g->len + (size_t)size > g->cap) {
        size_t newcap = (g->len + (size_t)size) * 2;
        uint8_t* np = realloc(g->p, newcap);
        if (!np) {
            free(g->p);
            g->p = NULL;
            g->cap = 0;
            g->len = 0;
            return;
        }
        g->p = np;
        g->cap = newcap;
    }
    if (!g->p) return;   /* prior allocation failure: drop remaining data */
    memcpy(g->p + g->len, data, (size_t)size);
    g->len += (size_t)size;
}

static void handle_screen(int cfd)
{
    struct growbuf g = { NULL, 0, 0 };
    if (!frame_buf) {
        send_response(cfd, 404, "text/plain", "no frame yet", 12);
        return;
    }
    /* GL rows are bottom-up: point stb at the last row, negative stride,
     * so the encoded PNG comes out top-down (right-side up). */
    stbi_write_png_to_func(png_append, &g, frame_w, frame_h, 3,
                           frame_buf + (size_t)(frame_h - 1) * frame_w * 3,
                           -frame_w * 3);
    if (g.p) {
        send_response(cfd, 200, "image/png", g.p, g.len);
        free(g.p);
    } else
        send_response(cfd, 500, "text/plain", "encode failed", 13);
}

/* Prop names indexed 0..NB_PROPS-1, matching the ITEM_* defines in colditz.h
 * exactly (verified against colditz.h:228-247; NB_PROPS is 16). */
static const char* prop_name[NB_PROPS] = {
    "none",               /* ITEM_NONE               0x00 */
    "lockpick",           /* ITEM_LOCKPICK            0x01 */
    "key_one",            /* ITEM_KEY_ONE             0x02 */
    "key_two",            /* ITEM_KEY_TWO             0x03 */
    "prisoner_uniform",   /* ITEM_PRISONERS_UNIFORM   0x04 */
    "guard_uniform",      /* ITEM_GUARDS_UNIFORM      0x05 */
    "pass",                /* ITEM_PASS               0x06 */
    "shovel",              /* ITEM_SHOVEL             0x07 */
    "pickaxe",             /* ITEM_PICKAXE            0x08 */
    "saw",                 /* ITEM_SAW                0x09 */
    "rifle",               /* ITEM_RIFLE              0x0A */
    "stone",               /* ITEM_STONE              0x0B */
    "candle",               /* ITEM_CANDLE            0x0C */
    "papers",               /* ITEM_PAPERS            0x0D */
    "stethoscope",          /* ITEM_STETHOSCOPE       0x0E */
    "inflatable_dummy",     /* ITEM_INFLATABLE_DUMMY  0x0F */
};

static const char* nation_name[NB_NATIONS] = { "british", "french", "american", "polish" };

/* Non-static globals defined in main.c:145; glut_keyboard()/glut_keyboard_up()
 * read/write these directly, and input_pump() below drives them the same
 * way a real keypress/key-release would. */
extern bool key_down[256], key_readonce[256];
extern uint8_t last_key_used;

/* Walk status, shared between handle_input/handle_state (need it early --
 * cancel-on-manual-input and the /state "walk" field, respectively) and
 * the full walk implementation (grid snapshot, BFS, walk_pump,
 * handle_walk) defined further down, right before handle_request. See
 * that block's header comment for the walk feature overview. */
typedef enum { WALK_IDLE, WALK_WALKING, WALK_ARRIVED, WALK_BLOCKED } walk_status_t;
static walk_status_t walk_status = WALK_IDLE;

static const char* walk_status_name(walk_status_t s)
{
    switch (s) {
        case WALK_WALKING: return "walking";
        case WALK_ARRIVED: return "arrived";
        case WALK_BLOCKED: return "blocked";
        default:            return "idle";
    }
}

/* Task 10 part B: WHY a walk ended blocked. Set alongside every
 * walk_cancel(WALK_BLOCKED) call site (via walk_set_blocked() below,
 * defined once the walk machinery's other state -- current_nation,
 * guybrush[], walk_is_exit_target/walk_target_x/y -- is in scope) so
 * /state's "walk_blocked_reason" field always reflects the actual cause of
 * the most recent blocked result, not just the fact that it happened.
 * Reset to NONE (serialized as JSON null) at the start of every fresh
 * /walk; /state only ever reports a non-null reason while walk=="blocked",
 * regardless of what this variable happens to hold (see
 * walk_blocked_reason_name's use in handle_state). */
typedef enum {
    WALK_BLOCK_NONE, WALK_BLOCK_GUARD, WALK_BLOCK_DOOR,
    WALK_BLOCK_STATIC, WALK_BLOCK_TIMEOUT
} walk_blocked_reason_t;
static walk_blocked_reason_t walk_blocked_reason = WALK_BLOCK_NONE;

static const char* walk_blocked_reason_name(walk_blocked_reason_t r)
{
    switch (r) {
        case WALK_BLOCK_GUARD:   return "\"guard\"";
        case WALK_BLOCK_DOOR:    return "\"door\"";
        case WALK_BLOCK_STATIC:  return "\"static\"";
        case WALK_BLOCK_TIMEOUT: return "\"timeout\"";
        default:                 return "null";
    }
}

/* Direction key codes resolved once at walk start (conf.h KEYVAL bindings
 * don't change mid-game, so caching avoids re-resolving them every tick --
 * see input_pump, which re-resolves per /input request instead since that
 * only happens once per queued key, not every tick). */
static uint8_t walk_key_up, walk_key_down, walk_key_left, walk_key_right;

/* Releases all four walk-held direction keys, mirroring glut_keyboard_up's
 * key_down=false/key_readonce=false pair exactly (see input_pump's own
 * release comment) so nothing about a walk-held key looks different, to
 * the rest of the engine, from a real key release. Safe to call when no
 * keys are actually held (e.g. walk_key_* still zero-initialized) --
 * releasing an already-released key is a no-op. */
static void walk_release_keys(void)
{
    key_down[walk_key_up] = false;    key_readonce[walk_key_up] = false;
    key_down[walk_key_down] = false;  key_readonce[walk_key_down] = false;
    key_down[walk_key_left] = false;  key_readonce[walk_key_left] = false;
    key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
}

/* Single choke point for ending a walk, on EVERY termination path (target
 * reached, room change, prisoner switch, stall/blocked, manual /cancel,
 * manual /input override, or a fresh /walk superseding this one): release
 * keys if we were actually holding them, then land on the given status.
 * Idempotent -- calling this when already idle/arrived/blocked just
 * re-releases (harmlessly) and re-sets the status. */
static void walk_cancel(walk_status_t new_status)
{
    if (walk_status == WALK_WALKING)
        walk_release_keys();
    walk_status = new_status;
}

#define INPUT_QUEUE_LEN 32
static struct { uint8_t code; int ticks; } input_q[INPUT_QUEUE_LEN];
static int q_head = 0, q_len = 0;
static int active_ticks = 0;          /* ticks left on current hold */

static int agent_input_queue_depth(void) { return q_len + (active_ticks>0); }

static uint8_t key_for_name(const char* name)
{
    if (!strcmp(name,"up"))        return KEY_DIRECTION_UP;
    if (!strcmp(name,"down"))      return KEY_DIRECTION_DOWN;
    if (!strcmp(name,"left"))      return KEY_DIRECTION_LEFT;
    if (!strcmp(name,"right"))     return KEY_DIRECTION_RIGHT;
    if (!strcmp(name,"action"))    return KEY_ACTION;
    if (!strcmp(name,"pickup"))    return KEY_INVENTORY_PICKUP;
    if (!strcmp(name,"drop"))      return KEY_INVENTORY_DROP;
    if (!strcmp(name,"inv_left"))  return KEY_INVENTORY_LEFT;
    if (!strcmp(name,"inv_right")) return KEY_INVENTORY_RIGHT;
    if (!strcmp(name,"walk_run"))  return KEY_TOGGLE_WALK_RUN;
    if (!strcmp(name,"sleep"))     return KEY_SLEEP;
    if (!strcmp(name,"stooge"))    return KEY_STOOGE;
    if (!strcmp(name,"pause"))     return KEY_PAUSE;
    if (!strcmp(name,"escape"))    return KEY_ESCAPE;
    if (!strcmp(name,"prisoner_1")) return KEY_PRISONER_BRITISH;
    if (!strcmp(name,"prisoner_2")) return KEY_PRISONER_FRENCH;
    if (!strcmp(name,"prisoner_3")) return KEY_PRISONER_AMERICAN;
    if (!strcmp(name,"prisoner_4")) return KEY_PRISONER_POLISH;
    return 0;
}

/* Minimal JSON string/int field extractors (fixed tiny schema, no lib). */
static bool json_str(const char* body, const char* field, char* out, size_t sz)
{
    char pat[32]; const char* p; size_t i = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    p = strstr(body, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':'); if (!p) return false;
    p = strchr(p, '"');               if (!p) return false;
    for (p++; *p && *p != '"' && i < sz-1; p++, i++) out[i] = *p;
    out[i] = '\0';
    return true;
}
static long json_int(const char* body, const char* field, long dflt)
{
    char pat[32]; const char* p;
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    p = strstr(body, pat);
    if (!p) return dflt;
    p = strchr(p + strlen(pat), ':'); if (!p) return dflt;
    return strtol(p+1, NULL, 10);
}

/* Appends one key press/hold to the input queue. Shared by handle_input
 * (arbitrary named key) and handle_control (KEY_PAUSE toggle). Returns
 * false, without touching the queue, if it's already full -- callers turn
 * that into a 400 rather than silently dropping the request, since a
 * silently-ignored /control pause would leave the caller believing the
 * game paused when it didn't. */
static bool enqueue_key(uint8_t code, int ms)
{
    int slot, ticks;
    if (q_len >= INPUT_QUEUE_LEN) return false;
    slot = (q_head + q_len) % INPUT_QUEUE_LEN;
    ticks = ms / 16;
    if (ticks < 1) ticks = 1;
    input_q[slot].code = code;
    input_q[slot].ticks = ticks;
    q_len++;
    return true;
}

static void handle_input(int cfd, const char* body)
{
    char name[24]; char resp[64]; uint8_t code; long ms; int n;
    if (!body || !json_str(body, "key", name, sizeof(name))) {
        send_response(cfd, 400, "text/plain", "missing \"key\"", 13);
        return;
    }
    code = key_for_name(name);
    if (code == 0) {
        static const char* valid = "valid keys: up down left right action "
            "pickup drop inv_left inv_right walk_run sleep stooge pause "
            "escape prisoner_1..4";
        send_response(cfd, 400, "text/plain", valid, strlen(valid));
        return;
    }
    ms = json_int(body, "ms", 100);
    if (ms < 16) ms = 16;
    if (ms > 10000) ms = 10000;
    /* A manual key overrides any in-progress walk: cancel it (releasing
     * the walk-held direction keys) before this key is enqueued, per the
     * brief -- otherwise the walk's held keys and this queued key would
     * fight over the same key_down[] state machine. */
    walk_cancel(WALK_IDLE);
    if (!enqueue_key(code, (int)ms)) {
        send_response(cfd, 400, "text/plain", "queue full", 10);
        return;
    }
    n = snprintf(resp, sizeof(resp), "{\"queued\":%d}",
                 agent_input_queue_depth());
    send_response(cfd, 202, "application/json", resp, n);
}

/* POST /control {"pause":bool} -> 200 {"paused":bool}. Idempotent: if the
 * game is already in the requested state, no key is injected (the `want !=
 * paused` guard below), so repeated identical calls never toggle it back
 * out. Pausing/unpausing goes through the game's own KEY_PAUSE key path
 * (via enqueue_key/input_pump, same mechanism as /input) rather than
 * poking game_state directly, so it stays in sync with everything else
 * the real key binding does (picture-fade, pause-screen render, etc).
 * See docs/AGENT-API.md for the resulting display caveat. */
static void handle_control(int cfd, const char* body)
{
    static const char* need_pause = "expected \"pause\"";
    char resp[48]; int n; const char* p; bool want;
    if (!body || !(p = strstr(body, "\"pause\""))) {
        send_response(cfd, 400, "text/plain", need_pause, strlen(need_pause));
        return;
    }
    want = (strstr(p, "true") != NULL);
    if (want != (paused ? true : false)) {
        /* Unpausing needs a much longer hold than pausing does. Pausing is
         * consumed immediately by user_input()'s read_key_once(KEY_PAUSE)
         * on the very next tick (main.c:587), so a short hold is plenty.
         * Unpausing, though, is only detected once the pause screen's own
         * fade state machine reaches PICTURE_WAIT (main.c glut_idle_static_
         * pic), which takes ~2*TRANSITION_DURATION (2000ms: one fade-out,
         * one fade-in, both 1000ms per colditz.h) to reach from the moment
         * pausing started -- and PICTURE_WAIT is the ONLY place that polls
         * key_down[KEY_PAUSE] again while paused (user_input(), and with
         * it every other KEY_PAUSE check, doesn't run at all while paused
         * -- see glut_idle_game's early return). A short injected press
         * (the original 100ms) releases well before that 2s mark if
         * unpause is requested soon after pause, so PICTURE_WAIT's
         * read_key_once() never sees it held and the game is stranded
         * paused forever (verified: game_time stays frozen indefinitely).
         * Holding for 2200ms comfortably bridges that window regardless of
         * when during the fade sequence the unpause request lands. */
        int ms = want ? 100 : 2200;
        if (!enqueue_key(KEY_PAUSE, ms)) {
            send_response(cfd, 400, "text/plain", "queue full", 10);
            return;
        }
    }
    n = snprintf(resp, sizeof(resp), "{\"paused\":%s}", want?"true":"false");
    send_response(cfd, 200, "application/json", resp, n);
}

/* POST /say {"text":"..."} -> 200. Displays `text` on the in-game status
 * bar via set_status_message(), which stores the POINTER it's given (it
 * doesn't copy) -- hence the persistent static buffer below rather than a
 * stack temporary. Priority 3 matches the highest priority used by any
 * existing call site (game.c debug/cheat messages; see game.h:89-96,
 * set_status_message only overwrites when priority >= current), so an
 * agent's commentary always wins over routine room/props messages instead
 * of being silently dropped by set_status_message's priority gate. The
 * game font renders plain ASCII; non-ASCII bytes in `text` are passed
 * through as-is (not stripped) -- see docs/AGENT-API.md. */
static char say_buf[128];
static void handle_say(int cfd, const char* body)
{
    if (!body || !json_str(body, "text", say_buf, sizeof(say_buf))) {
        send_response(cfd, 400, "text/plain", "missing \"text\"", 14);
        return;
    }
    set_status_message(say_buf, 3, 4000);
    send_response(cfd, 200, "application/json", "{\"ok\":true}", 11);
}

/* Called every tick from agent_api_tick(): advance the input machine. */
static void input_pump(void)
{
    static uint8_t active_code = 0;
    if (active_ticks > 0) {
        if (--active_ticks == 0) {           /* release, like glut_keyboard_up */
            key_down[active_code] = false;
            key_readonce[active_code] = false;
        }
        return;                              /* one key at a time */
    }
    if (q_len > 0) {
        active_code = input_q[q_head].code;
        active_ticks = input_q[q_head].ticks;
        q_head = (q_head + 1) % INPUT_QUEUE_LEN;
        q_len--;
        key_down[active_code] = true;        /* press, like glut_keyboard */
        last_key_used = active_code;         /* intro/menus exit on this */
    }
}

/* Status bar text, JSON-sanitized. Exported directly as `status_message`
 * (colditz.h:745: extern char *status_message;) -- no accessor needed. */
static const char* agent_status_message(void)
{
    static char clean[128];
    const char* s = status_message ? status_message : "";
    int i;
    for (i = 0; s[i] && i < 127; i++)
        clean[i] = (s[i]=='"' || s[i]=='\\' || (unsigned char)s[i]<0x20)
                   ? ' ' : s[i];
    clean[i] = '\0';
    return clean;
}

/* Appends to buf at offset n, clamped to bufsz. snprintf's return value is
 * the length it WOULD have written on truncation, which is unbounded by
 * bufsz; blindly doing `n += snprintf(...)` lets n exceed bufsz, and a
 * later `bufsz - n` (both size_t/int mixed) wraps to a huge unsigned value
 * feeding straight back into snprintf's size argument -- an out-of-bounds
 * write. This clamps n to never exceed bufsz, so buf+n is always at worst
 * one-past-the-end (never dereferenced) and remaining size is always >= 0. */
static int json_append(char* buf, size_t bufsz, int n, const char* fmt, ...)
{
    size_t remain;
    int w;
    va_list ap;
    if (n < 0 || (size_t)n >= bufsz)
        return (int)bufsz;             /* already full */
    remain = bufsz - (size_t)n;
    va_start(ap, fmt);
    w = vsnprintf(buf + n, remain, fmt, ap);
    va_end(ap);
    if (w < 0)
        return n;                      /* encoding error: no progress */
    if ((size_t)w >= remain)
        return (int)bufsz;             /* truncated: treat buffer as full */
    return n + w;
}

static int json_prisoner(char* p, size_t sz, int i)
{
    int n = 0, j;
    bool first;
    n = json_append(p, sz, n,
        "{\"nation\":\"%s\",\"room\":%d,\"x\":%d,\"y\":%d,"
        "\"direction\":%d,\"speed\":%d,\"state_flags\":%u,"
        "\"dressed_as_guard\":%s,\"fatigue\":%u,\"escaped\":%s,\"dead\":%s,"
        "\"inventory\":{",
        nation_name[i], guybrush[i].room, guybrush[i].px, guybrush[i].p2y/2,
        guybrush[i].direction, guybrush[i].speed, (unsigned)guybrush[i].state,
        guybrush[i].is_dressed_as_guard?"true":"false",
        (unsigned)p_event[i].fatigue, p_event[i].escaped?"true":"false",
        p_event[i].killed?"true":"false");
    first = true;
    for (j = 1; j < NB_PROPS; j++)
        if (props[i][j] > 0) {
            n = json_append(p, sz, n, "%s\"%s\":%u", first?"":",",
                             prop_name[j], (unsigned)props[i][j]);
            first = false;
        }
    n = json_append(p, sz, n, "},\"selected\":\"%s\"}",
                     prop_name[selected_prop[i]]);
    return n;
}

static void handle_state(int cfd)
{
    static char json[8192];
    int n = 0, i;
    n = json_append(json, sizeof(json), n,
        "{\"game_time\":%llu,\"paused\":%s,\"menu\":%s,\"intro\":%s,"
        "\"current_prisoner\":%u,\"input_queue\":%d,\"prisoners\":[",
        (unsigned long long)game_time, paused?"true":"false",
        game_menu?"true":"false", intro?"true":"false",
        (unsigned)current_nation, agent_input_queue_depth());
    for (i = 0; i < NB_NATIONS; i++) {
        if (i)
            n = json_append(json, sizeof(json), n, ",");
        /* json_prisoner writes directly into the remaining tail of `json`
         * (sized to exactly what's left) and internally clamps via
         * json_append, so n + its return value can never exceed
         * sizeof(json). */
        n += json_prisoner(json+n, sizeof(json)-(size_t)n, i);
    }
    n = json_append(json, sizeof(json), n,
                     "],\"message\":\"%s\",\"walk\":\"%s\",\"walk_blocked_reason\":%s}",
                     agent_status_message(), walk_status_name(walk_status),
                     (walk_status == WALK_BLOCKED)
                         ? walk_blocked_reason_name(walk_blocked_reason) : "null");
    send_response(cfd, 200, "application/json", json, (size_t)n);
}

/* Largest room we will ever serve: the outside map is CMP_MAP_WIDTH(84) x
 * CMP_MAP_HEIGHT(72) = 6048 tiles. This cap is checked against room_x*room_y
 * right after set_room_xy() and rejects both that legitimate max (safely,
 * with headroom) and any garbage width/height read out of a tunnel room
 * (per the brief: tunnel rooms can read garbage through these macros). */
#define ROOM_MAX_TILES 8192

/* Dedicated 32 KB static response buffer -- the 8 KB /state buffer (`json`
 * above) is too small: the outside grid alone is ~74 rows * ~88 chars
 * (width + quotes/comma) =~ 6.5 KB, plus the exits array and other fields. */
static char room_buf[32768];

/* GET /room: the current room's *visible* geometry only -- walkable floor
 * grid, exit tile coordinates, the prisoner's own tile, and (Task 9) the
 * current room's visible item props. Fair-play mandate (user-specified,
 * non-negotiable): never expose anything a human player can't see on
 * screen. In particular this never reads/emits door locked/open flags,
 * key grades, or any other-room data -- an agent learns whether a door is
 * locked the same way a human does, by trying it. Room props ARE exposed
 * (name + tile only, see the `items` block below) because they're
 * rendered on screen exactly like the floor grid is -- reading their
 * name/position is no more of a look-ahead than /room's grid already is;
 * what stays hidden is any lock/hidden/other-room state, same as for
 * exits. We also only ever serve the CURRENT room: the readtile()/readexit()
 * macros key off is_outside, which itself tests current_room_index, so
 * serving an arbitrary room index would desync is_outside from the data
 * actually being read (and would also let an agent see rooms it hasn't
 * been in, which is its own flavor of cheating) -- so no room parameter
 * is accepted. */
static void handle_room(int cfd)
{
    uint16_t room = guybrush[current_nation].room;
    bool outside = (room == ROOM_OUTSIDE);
    /* CAUTION: room_x/room_y/offset are the engine's own working globals,
     * reused elsewhere for the engine's own mid-frame bookkeeping. We save
     * them before calling set_room_xy() and restore them before every
     * return past that point, so this request never disturbs engine state
     * the rest of the frame (or the next callback) depends on. */
    uint16_t saved_room_x = room_x, saved_room_y = room_y;
    uint32_t saved_offset = offset;
    uint16_t width, height;
    int16_t tile_x, tile_y;
    int n = 0, x, y;
    uint16_t u;
    bool first;

    if (!outside) {
        /* Mirror set_room_xy()'s own offset computation (game.c:717) to
         * detect the 0xFFFFFFFF CRM-gap sentinel BEFORE calling
         * set_room_xy() and touching the shared globals -- set_room_xy()
         * itself does not check this (see its comment at game.c:718-719). */
        uint32_t raw = readlong((uint8_t*)fbuffer[ROOMS],
                                CRM_OFFSETS_START + 4*(uint32_t)room);
        if (raw == 0xFFFFFFFF) {
            send_response(cfd, 500, "text/plain",
                          "room data unavailable", 21);
            return;
        }
    }

    set_room_xy(room);
    width = room_x;
    height = room_y;

    /* Reject zero-sized or implausibly large dimensions (garbage read from
     * a tunnel room, or any future corrupt data) before indexing anything,
     * rather than trusting engine data blindly. */
    if (width == 0 || height == 0 ||
        (size_t)width * (size_t)height > ROOM_MAX_TILES) {
        room_x = saved_room_x; room_y = saved_room_y; offset = saved_offset;
        send_response(cfd, 500, "text/plain", "room data unavailable", 21);
        return;
    }

    tile_x = guybrush[current_nation].px / 32;
    tile_y = guybrush[current_nation].p2y / 32;
    if (tile_x < 0) tile_x = 0;
    if (tile_y < 0) tile_y = 0;
    if (tile_x >= (int16_t)width)  tile_x = (int16_t)width  - 1;
    if (tile_y >= (int16_t)height) tile_y = (int16_t)height - 1;

    n = json_append(room_buf, sizeof(room_buf), n,
        "{\"room\":%d,\"outside\":%s,\"width\":%d,\"height\":%d,"
        "\"my_tile\":[%d,%d],\"grid\":[",
        (int)room, outside?"true":"false", (int)width, (int)height,
        (int)tile_x, (int)tile_y);

    /* grid: one string per row (y=0 first). '.'=void (tile id 0), '#'=floor
     * (any nonzero tile id -- a coarse walkable test; furniture/walls
     * within a nonzero tile can still block at pixel level, documented in
     * AGENT-API.md), 'E'=exit cell. We deliberately read ONLY the exit
     * index's low 5 bits' nonzero-ness (readexit(x,y) & 0x1F) to know
     * "this cell is a doorway/stair" -- never the locked/grade byte, which
     * lives at a different offset entirely and is never touched here. */
    for (y = 0; y < (int)height; y++) {
        n = json_append(room_buf, sizeof(room_buf), n, "%s\"", y ? "," : "");
        for (x = 0; x < (int)width; x++) {
            char c = readtile(x, y) ? '#' : '.';
            if ((readexit(x, y) & 0x1F) != 0)
                c = 'E';
            n = json_append(room_buf, sizeof(room_buf), n, "%c", c);
        }
        n = json_append(room_buf, sizeof(room_buf), n, "\"");
    }
    n = json_append(room_buf, sizeof(room_buf), n, "],\"exits\":[");

    first = true;
    for (y = 0; y < (int)height; y++) {
        for (x = 0; x < (int)width; x++) {
            if ((readexit(x, y) & 0x1F) != 0) {
                n = json_append(room_buf, sizeof(room_buf), n,
                    "%s{\"tile\":[%d,%d]}", first ? "" : ",", x, y);
                first = false;
            }
        }
    }
    n = json_append(room_buf, sizeof(room_buf), n, "],\"items\":[");

    /* Items: the CURRENT room's pickable props, by name+tile -- the same
     * data set_props_overlays() (game.c:954-1005) draws every frame, read
     * the same way. nb_room_props/room_props[] are already scoped to
     * current_room_index (== guybrush[current_nation].room, see colditz.h's
     * #define) by set_room_props(), refreshed on every room/nation switch
     * -- so, like readtile/readexit, this is only ever self-consistent for
     * the room we already validated above; no extra room-index handling
     * needed. Fair-play: only name (via the same prop_name[] table /state
     * already exposes) and tile position -- no lock/hidden/other-room
     * state, and nothing here is data a human standing in this room
     * couldn't also see on screen. */
    first = true;
    for (u = 0; u < nb_room_props; u++) {
        uint16_t prop_offset = room_props[u];
        uint8_t item_id;
        uint16_t raw_x, raw_y;
        int16_t itile_x, itile_y, anchor_x, anchor_y;

        if (prop_offset == 0)
            continue;   /* picked up since the last set_room_props() call */

        item_id = readbyte(fbuffer[OBJECTS], prop_offset + 7);
        if (item_id == ITEM_NONE || item_id >= NB_PROPS)
            continue;   /* defensive: never index prop_name[] out of range */

        /* Same pixel-position formula set_props_overlays() uses for its
         * own draw position (x = raw_x-15, y = raw_y-4), converted to tile
         * coordinates the same way my_tile is (divide by the tile pixel
         * size on each axis -- 32 for x, 16 for y since these words store
         * a py-equivalent, not p2y, value: main.c's drop handler writes
         * prisoner_2y/2+4 here, matching prop_offset+2's read-back). This
         * is also the EXACT engine pickup-trigger anchor (game.c:962-994,
         * check_footprint's over_prop test): prisoner_x in [x-9,x+8) and
         * prisoner_2y/2 in [y-9,y+8), the same room-pixel-coordinate space
         * /state's per-prisoner x/y fields use -- exposed below as `x`/`y`
         * so a caller can walk to pixel precision instead of only tile
         * precision (which is too coarse: a prop can sit off a tile's
         * center by furniture-sized margins). */
        raw_x = readword(fbuffer[OBJECTS], prop_offset + 4);
        raw_y = readword(fbuffer[OBJECTS], prop_offset + 2);
        anchor_x = (int16_t)(raw_x - 15);
        anchor_y = (int16_t)(raw_y - 4);
        itile_x = (int16_t)((raw_x - 15) / 32);
        itile_y = (int16_t)((raw_y - 4) / 16);

        /* Outside map only: a prop can be covered by a removable wall,
         * exactly the check set_props_overlays() makes before adding its
         * overlay (game.c:1001) -- skip it here too so /room never reports
         * a prop the renderer wouldn't actually draw. */
        if (outside) {
            if (itile_x < 0 || itile_y < 0 ||
                itile_x >= CMP_MAP_WIDTH || itile_y >= CMP_MAP_HEIGHT)
                continue;
            if (remove_props[itile_x][itile_y])
                continue;
        }

        if (itile_x < 0) itile_x = 0;
        if (itile_y < 0) itile_y = 0;
        if (itile_x >= (int16_t)width)  itile_x = (int16_t)width  - 1;
        if (itile_y >= (int16_t)height) itile_y = (int16_t)height - 1;

        n = json_append(room_buf, sizeof(room_buf), n,
            "%s{\"name\":\"%s\",\"tile\":[%d,%d],\"x\":%d,\"y\":%d}",
            first ? "" : ",", prop_name[item_id],
            (int)itile_x, (int)itile_y, (int)anchor_x, (int)anchor_y);
        first = false;
    }
    n = json_append(room_buf, sizeof(room_buf), n, "]}");

    room_x = saved_room_x;
    room_y = saved_room_y;
    offset = saved_offset;

    /* json_append() clamps rather than overflows, but a fully-clamped
     * (truncated) buffer would be invalid/misleading JSON; refuse to send
     * it rather than silently truncate. Given ROOM_MAX_TILES above this
     * should never actually trigger for real room/outside data -- it's a
     * defense-in-depth backstop, not the primary size guard. */
    if (n >= (int)sizeof(room_buf)) {
        send_response(cfd, 500, "text/plain", "room data unavailable", 21);
        return;
    }

    send_response(cfd, 200, "application/json", room_buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* POST /walk: autonomous in-room pathing, hybrid of handle_room's grid
 * access (fair-play: same visible-floor-only geometry) and input_pump's
 * key-hold state machine. See docs/AGENT-API.md for the fair-play note:
 * BFS treats every nonzero tile (including exit cells) as walkable, so a
 * walk can be accepted toward, and end blocked at, a locked door -- no
 * door/exit status is ever read here, same mandate as /room.
 *
 * Task 9 additions (see the WALK_PHASE_* block further down for the
 * design overview): (A) an exit-tile walk doesn't stop at the doorway's
 * threshold -- reaching it enters a CROSSING phase that holds an outward
 * direction key until the room actually changes, so `arrived` on an exit
 * walk now always means the room changed. (B) a stall on a non-final
 * waypoint gets one sidestep-and-re-path recovery attempt before being
 * declared blocked. (C) the whole walk (any combination of phases) is
 * capped at 30s wall-clock. */

/* Largest grid the BFS ever has to path over: the outside compressed map
 * (CMP_MAP_WIDTH x CMP_MAP_HEIGHT = 84x72). Real indoor rooms are far
 * smaller (the /room example in the docs is 20x12); walk_snapshot_grid()
 * rejects anything wider/taller than this before it's ever indexed. */
#define WALK_MAX_W 84
#define WALK_MAX_H 72
#define WALK_MAX_CELLS (WALK_MAX_W * WALK_MAX_H)

/* Task 10: sub-tile grid resolution (see the "mask-accurate sub-tile
 * pathing" block further down for the mask-sampling design). Defined here,
 * ahead of walk_path_x/y below, so their capacity can be sized to the
 * subcell grid's worst case (a raw pre-smoothing BFS path can in principle
 * visit every cell in the grid) rather than the coarser tile grid's. */
/* 8x8 subcells per 32x32 tile (4-unit cells) rather than the brief's
 * suggested 4x4 (8-unit cells): verified live during this task that 8-unit
 * cells alias badly against this engine's real footprint (SPRITE_FOOTPRINT
 * is ~12 real px wide, and the per-row vertical scan check_footprint does
 * is ~4 real px tall -- see mask_footprint_blocked's header comment) --
 * two adjacent 8-unit-spaced sample points can each individually read
 * "open" while a real wall/furniture edge sits entirely in the untested
 * gap between them, and the reverse (a single off-center-but-still-in-
 * deadband point colliding while the cell's own center doesn't). 4-unit
 * cells shrink that untested gap to less than the footprint's own
 * vertical scan step, which eliminates the specific stuck-mid-corridor
 * failures observed with 8-unit cells in this room's tightest passage
 * (see the Task 10 report's self-review for the concrete case and the
 * timing measurement that justified accepting the resulting 4x cell-count
 * increase). */
#define SUB_PER_TILE 8                    /* 8x8 subcells per 32x32 tile */
#define SUB_SIZE (32 / SUB_PER_TILE)      /* 4 -- cell size in px/p2y units */
#define WALK_SUB_MAX_W (WALK_MAX_W * SUB_PER_TILE)   /* 336 */
#define WALK_SUB_MAX_H (WALK_MAX_H * SUB_PER_TILE)   /* 288 */
#define WALK_SUB_MAX_CELLS ((int32_t)WALK_SUB_MAX_W * (int32_t)WALK_SUB_MAX_H) /* ~96.8k */

/* Walkable-floor snapshot (row-major, index y*width+x), taken once when a
 * /walk is accepted and read-only from then on -- the BFS and the pump
 * never re-touch engine state, so a walk in progress is immune to
 * mid-walk engine reads/writes happening elsewhere in the same tick. */
static bool walk_grid_walkable[WALK_MAX_CELLS];

/* Parallel snapshot: which cells are exit/doorway cells (readexit(x,y) &
 * 0x1F != 0), taken at the same time as walk_grid_walkable[] by the same
 * scan in walk_snapshot_grid(). Read-only after that, same as
 * walk_grid_walkable[] -- used by the CROSSING-phase outward-direction
 * heuristic (walk_exit_dir_candidates) so it never has to re-touch engine
 * state (room_x/room_y/offset) mid-walk. Only the exit index's low 5 bits'
 * nonzero-ness, same fair-play test /room and walk_grid_walkable already
 * use -- never door/exit status (locked/grade). */
static bool walk_grid_isexit[WALK_MAX_CELLS];

/* Dimensions of the walk_grid_walkable/isexit snapshot currently in
 * effect, persisted (not just a handle_walk local) so the CROSSING and
 * SIDESTEP-recovery phases -- which run over multiple ticks, long after
 * handle_walk returned -- can keep indexing the same snapshot correctly. */
static uint16_t walk_width = 0, walk_height = 0;

/* Waypoint path (tiles strictly after the start tile, through the target
 * inclusive), and the pump's cursor into it. int16_t is ample: tile
 * coordinates never exceed WALK_MAX_W/H. */
static int16_t walk_path_x[WALK_SUB_MAX_CELLS];
static int16_t walk_path_y[WALK_SUB_MAX_CELLS];
static int walk_path_len = 0;
static int walk_path_idx = 0;

/* The walk's ultimate target tile, persisted independent of walk_path_*
 * (which gets overwritten by a mid-walk stall-recovery re-path) so a
 * recovery attempt always re-BFS's toward the *original* target, and so
 * the CROSSING-phase direction heuristic has a stable (ex,ey) even for a
 * zero-length path (target tile == start tile, e.g. an {"exit":N} request
 * issued while already standing on that exit tile). */
static int16_t walk_target_x = 0, walk_target_y = 0;
static int16_t walk_start_x = 0, walk_start_y = 0;

/* Task 10: the mask-validated WALKABLE subcell within the target tile
 * (walk_resolve_target_subcell's result at accept/re-path time -- see its
 * own header comment), used by WALK_PHASE_PATH's steering as the aim
 * point for ONLY the walk's FINAL waypoint (see walk_pump). A doorway or
 * other partially-blocked tile's real opening is not always centered in
 * the tile (readtile/readexit define the tile, not where within its 32x32
 * footprint the floor actually is) -- verified live during this task: a
 * room-224 exit tile whose geometric center sits on a wall pixel made
 * PATH-phase steer toward an unreachable point forever (single manual
 * key-holds moved the prisoner fine; the deadband math was just aiming at
 * the wrong spot). Intermediate (non-final) waypoints keep targeting
 * their tile's plain geometric center, matching the pre-Task10 design --
 * ordinary interior floor tiles are essentially always fully open, so
 * there's no benefit (and the usual tight-corridor risk, see walk_bfs_sub's
 * header comment) to steering them any more precisely than that. */
static int16_t walk_target_sub_x = 0, walk_target_sub_y = 0;

/* True when the walk's target is an exit/doorway cell (set at accept time
 * in handle_walk, from either an explicit {"exit":N} request or a
 * {"tile":[x,y]} landing on walk_grid_isexit[]) -- gates whether reaching
 * the target tile enters the CROSSING phase (part A) or ends the walk
 * immediately as arrived (plain-tile walks, unchanged Task 8 behavior). */
static bool walk_is_exit_target = false;

/* True for an item-mode walk ({"item":"<name>"}, the pickup feature) --
 * gates whether reaching the BFS target tile hands off to the pixel-
 * precision ITEM_PIXEL phase instead of ending the walk immediately.
 * Mutually exclusive with walk_is_exit_target (handle_walk always forces
 * the exit-target test off for an item walk -- reaching a prop's tile is
 * never a doorway crossing). walk_item_anchor_x/y are the prop's exact
 * pickup-trigger pixel anchor (same room-pixel-coordinate space as /state's
 * x/y, computed once at accept time via the game.c:962-994 formula -- see
 * handle_room's `items` block for the identical computation). */
static bool walk_is_item_target = false;
static bool walk_item_pickup = false;
static int16_t walk_item_anchor_x = 0, walk_item_anchor_y = 0;

/* ITEM_PIXEL phase's per-axis progress tracker (ordinary, non-rounding
 * steering): tracks the best (smallest) distance-to-target seen on
 * whichever axis is currently "primary" (the one being actively steered,
 * x taking priority over y -- see walk_pump_item_pixel), and how many
 * ticks it's been since that best was improved. Reset whenever the
 * primary axis switches (x settling into its deadband and handing off to
 * y, or vice versa) so a fresh window always applies to whichever axis is
 * currently being pushed. Hitting WALK_ITEM_PROGRESS_TICKS with no
 * improvement is the furniture-corner-stall trigger for a corner-rounding
 * round (see walk_pump_item_round below) -- deliberately far more
 * sensitive than the whole-walk WALK_STALL_LIMIT every other phase uses:
 * a pinned prisoner can keep sliding along a blocked axis (technically
 * moving a little most ticks, from collision-response jitter) while never
 * actually getting closer, so literal zero-movement detection misses the
 * stall entirely -- exactly what let the walker sit pinned against a bed
 * for 13+ frames in the film that motivated this feature. */
static bool walk_item_progress_axis_is_x = false;
static int16_t walk_item_progress_best = 0;
static int walk_item_progress_ticks = 0;
#define WALK_ITEM_PROGRESS_TICKS 10   /* ~0.16s -- brief, verbatim ("~10 ticks") */

/* Corner-rounding: a bounded sequence of explicit sidestep-then-re-attempt
 * rounds (brief, verbatim: "sidestep PERPENDICULAR ~12px one side; if
 * still no progress after re-attempt, ~24px the other side; re-attempt.
 * Max 3 corner-rounding rounds"), NOT a fixed-tick-count shift/push zigzag
 * with an exact-position-equality "did that help?" test -- an earlier
 * version of this phase worked that way and was verified live (room 251's
 * lockpick against a bunk bed) to get stuck retrying the wrong side
 * forever: a blocked push still causes a little collision-response
 * jitter most cycles, so exact-equality almost never true, so the side
 * never flipped away from the wrong one. This version MEASURES actual
 * pixel displacement for the sidestep step, and judges the re-attempt
 * step by the same best-distance progress test ordinary steering uses
 * (above), so both stages decide "did that help?" the same principled
 * way. Each round: (1) hold ONLY a perpendicular direction until measured
 * perpendicular displacement reaches that round's target pixel count (or
 * a safety tick cap, in case the perpendicular direction is ALSO
 * blocked), (2) hold ONLY the still-blocked primary direction and watch
 * for progress. Progress -> round succeeds, drop back to ordinary
 * steering with a fresh progress window (the whole-walk round budget is
 * NOT reset, so a later stall picks up where this one left off). No
 * progress -> next round: bigger sidestep, alternating sides (round 1:
 * 12px side A; round 2: 24px side B; round 3: 24px side A again, in case
 * A was the right side but round 1's 12px undershot the corner).
 * Exhausting WALK_ITEM_ROUND_MAX rounds falls through to the SAME
 * generic sidestep-then-re-BFS recovery plain-tile PATH-phase stalls use
 * (walk_recovery_count, shared, capped at WALK_MAX_RECOVERIES per whole
 * walk -- see that constant's own comment for why it's 2, not the brief's
 * original 1) -- brief, verbatim: "then the existing one-shot re-path
 * recovery -> then blocked" (now a small bounded number of attempts
 * rather than exactly one; see WALK_MAX_RECOVERIES). The
 * primary axis for a whole round is captured once at the round's start
 * (walk_item_round_primary_is_x) rather than recomputed every tick, so a
 * momentary axis flip mid-round (brief: "temporary leaving of an in-range
 * axis during rounding is allowed") doesn't reinterpret which direction
 * is being sidestepped partway through. */
#define WALK_ITEM_ROUND_MAX 3   /* per approach side; matches the brief's 3-round design */
#define WALK_ITEM_ROUND_REATTEMPT_TICKS 10
#define WALK_ITEM_ROUND_SIDESTEP_CAP_TICKS 80   /* ~1.28s safety cap if the perpendicular direction is itself blocked */
static const int16_t walk_item_round_target_px[WALK_ITEM_ROUND_MAX] = { 12, 24, 24 };
static int walk_item_round = 0;                    /* rounds started so far this walk (whole-walk budget) */
static bool walk_item_rounding = false;             /* currently executing a round */
static bool walk_item_round_sidestepping = false;   /* true: sidestep sub-step; false: re-attempt sub-step */
static bool walk_item_round_primary_is_x = false;   /* primary axis, frozen for the round's duration */
static int walk_item_round_side = 0;                /* which perpendicular side this round tries */
static int16_t walk_item_round_ref_px = 0, walk_item_round_ref_p2y = 0;  /* position when the current sub-step began */
static int walk_item_round_ticks = 0;               /* ticks elapsed in the current sub-step */
static int16_t walk_item_round_best_dist = 0;       /* primary-axis distance when the re-attempt sub-step began */

/* Deadband (+-px) around the item anchor: ordinary steering stops
 * actively correcting an axis once it's this close, handing primary
 * steering off to the other axis. Looser +-WALK_ITEM_MARGIN is used only
 * for the arrival test, so an axis that's stopped being corrected is
 * already guaranteed to satisfy arrival too. The engine's own
 * pickup-trigger window (game.c:962-994) is prisoner_x-anchor_x in
 * [-9,+8) and prisoner_2y/2-anchor_y in [-9,+8) -- a 17-wide window;
 * +-7 sits strictly inside it on both sides, so "arrived" here always
 * implies the engine's own pickup test also passes. */
#define WALK_ITEM_DEADBAND 6
#define WALK_ITEM_MARGIN 7

/* Approach-side cycling for item walks: the prop's own tile is often
 * tile-walkable while furniture pixel-blocks the pickup window from some
 * sides (live case: room 251's lockpick is unreachable from the bed's
 * south face but reachable from the east). When corner-rounding exhausts
 * on one approach, re-BFS to the next untried walkable 4-neighbor of the
 * prop tile and re-run the pixel approach from there; only when every
 * side has been tried does the walk fall through to the generic recovery
 * and then blocked. */
static int16_t walk_item_prop_tx = -1, walk_item_prop_ty = -1;
static uint8_t walk_item_tried_mask = 0;   /* bit i = neighbor (E,W,S,N)[i] attempted */

/* Snapshot of "who/where" taken at walk start, so the pump can detect a
 * prisoner switch or room change without re-reading engine globals it
 * doesn't otherwise need. */
static uint8_t walk_nation = 0;
static uint16_t walk_room = 0;

/* Blocked (no-progress) detection: if px/p2y haven't moved for
 * WALK_STALL_LIMIT consecutive ticks (~40 * 16ms = 0.64s) while direction
 * keys are held, treat it as a locked door / furniture / guard body-block
 * -- exactly what a human bumping into an obstacle experiences. */
#define WALK_STALL_LIMIT 40
static int16_t walk_stall_px = 0, walk_stall_p2y = 0;
static int walk_stall_ticks = 0;

/* Deadband (+-units) around a waypoint tile's center so the pump doesn't
 * flap direction keys on/off every tick once the prisoner is already
 * close enough to center. WALK_PHASE_PATH's own steering, WALK_PHASE_CROSS's
 * perpendicular-axis correction, and SIDESTEP's primary-direction-vs-tile-
 * center comparisons all use this unchanged: every waypoint driven by
 * key-holding steering is a TILE (32-unit) target (see walk_bfs_sub's
 * header comment for why -- an earlier version of this task drove
 * PATH-phase at raw SUBCELL resolution instead, which needed, and got, a
 * much smaller deadband; that whole approach was reverted after live
 * testing found it unreliable, so a subcell-scaled deadband is no longer
 * needed here). */
#define WALK_DEADBAND 6

/* ---- Task 9: walk-through exits (A) + stall auto-recovery (B) ----
 *
 * The pump's tick state machine gains two extra phases beyond the
 * original "follow the BFS path" behavior (kept as WALK_PHASE_PATH):
 *
 *  - WALK_PHASE_CROSS: entered when the target tile of an exit walk is
 *    reached. Holds an "outward" direction key (see
 *    walk_exit_dir_candidates below) until the room changes (arrived) or
 *    the crossing attempt's time budget runs out (blocked).
 *  - WALK_PHASE_SIDESTEP: entered on a stall while still en route to a
 *    non-final waypoint. Nudges perpendicular to the current leg's
 *    direction of travel for a bounded time, then re-BFS's from wherever
 *    that left the prisoner to the ORIGINAL target on a fresh grid
 *    snapshot, and resumes WALK_PHASE_PATH. A small bounded number of
 *    recoveries per walk (WALK_MAX_RECOVERIES, walk_recovery_count counts
 *    attempts used -- see that constant's own comment); a stall once the
 *    budget is exhausted (or a stall on the final waypoint) is blocked
 *    immediately, as before Task 9.
 *
 * All three phases terminate exclusively through walk_cancel() (called
 * from walk_pump's shared per-tick preamble below), so the single
 * key-release choke point from Task 8 still holds for every new
 * termination path this adds.
 *
 * A fourth phase, WALK_PHASE_ITEM_PIXEL (the /walk item+pickup feature),
 * follows the same rule: entered when a PATH-phase item walk reaches its
 * target tile, it steers to pixel precision and terminates exclusively
 * through walk_cancel() too. Its own stall recovery is a dedicated bounded
 * corner-rounding sequence (see walk_pump_item_round's header comment);
 * only once THAT is exhausted does it fall through to WALK_PHASE_SIDESTEP
 * itself, reusing the exact same walk_recovery_count-gated
 * sidestep-then-re-BFS plain-tile stalls use -- on that path's re-BFS
 * completing, PATH-phase's own target-reached check routes back into
 * ITEM_PIXEL. */
typedef enum {
    WALK_PHASE_PATH, WALK_PHASE_CROSS, WALK_PHASE_SIDESTEP,
    WALK_PHASE_ITEM_PIXEL
} walk_phase_t;
static walk_phase_t walk_phase = WALK_PHASE_PATH;

/* Recovery-attempt budget per whole walk (part B); reset in handle_walk.
 * Originally a one-shot bool ("one recovery attempt per walk," per the
 * brief). Root-caused during the gauntlet stabilization pass: room 227's
 * 227->230 exit walk (target tile [3,5]) is a genuine two-recovery case --
 * live diagnostic logging (fprintf at both the stall-limit-hit and
 * sidestep_finish call sites, since removed) showed the SAME walk needing
 * sidestep-then-re-BFS recovery TWICE in sequence (first near tile (2,5)/
 * (1,3), then again near (2,4)) before the remaining leg to (3,5) was a
 * single straight, un-stalled run -- with the budget capped at 1, the
 * second stall had no recovery left and gave up with blocked_reason
 * "static" even though the walk was making genuine incremental progress
 * each time (not oscillating -- confirmed no other guybrush was ever in
 * the room at either stall, ruling out a guard encounter). A single retried
 * /walk from the same failure point succeeded immediately every time,
 * because a *fresh* walk resets this budget -- i.e. the walk fundamentally
 * only needed a bigger budget, not different logic. Raised to 2 (still a
 * small, bounded cap, not "unbounded retries") -- since this can only let
 * a walk that would previously have given up try harder, it cannot regress
 * any walk that only ever needed 0 or 1 recovery (room 251's lockpick,
 * room 253's [1,7]/[3,7] doorways, etc. -- all reverified unaffected). */
#define WALK_MAX_RECOVERIES 2
static int walk_recovery_count = 0;

/* Whole-walk (all phases combined) hard cap: 30s / 16ms/tick. Protects
 * against any pathological loop across path-following, sidestep-recovery,
 * and crossing all taking their maximum time in sequence. */
#define WALK_MAX_TICKS (30000 / 16)
static int walk_total_ticks = 0;

/* Cardinal direction, used by both the CROSSING and SIDESTEP phases to
 * name which single key is held. */
typedef enum { WALK_DIR_UP, WALK_DIR_DOWN, WALK_DIR_LEFT, WALK_DIR_RIGHT } walk_dir_t;

/* CROSSING phase state: up to 2 candidate outward directions (see
 * walk_exit_dir_candidates), alternated a bounded number of rounds. */
#define WALK_CROSS_LEG_TICKS 50   /* ~0.8s per candidate direction */
#define WALK_CROSS_MAX_ROUNDS 2
static walk_dir_t walk_cross_cand[2];
static int walk_cross_ncand = 0;
static int walk_cross_idx = 0;
static int walk_cross_round = 0;
static int walk_cross_ticks = 0;

/* SIDESTEP (recovery) phase state: try one perpendicular side, then the
 * other, each for a bounded time, then -- since the engine rejects a
 * blocked diagonal (both axes held at once, as PATH-phase's ordinary
 * steering does) as a single atomic move even when either axis ALONE is
 * free (verified live: room 251's post-pickup corner lets a single held
 * "right" cross a full tile boundary, and a single held "down" then clears
 * the furniture entirely, while holding both together never moves the
 * prisoner at all) -- a third leg retries the ORIGINAL blocked primary
 * direction alone (single-axis, matching the perpendicular legs' own
 * single-key discipline) before giving up and re-pathing. This mirrors the
 * item-pixel phase's own sidestep-then-reattempt design (see
 * walk_item_start_round's header comment) for the same reason: a
 * furniture corner needs single-axis probing to round, not a simultaneous
 * two-axis push. */
#define WALK_SIDESTEP_LEG_TICKS 25   /* ~0.4s per side */
static walk_dir_t walk_sidestep_dir[2];
static walk_dir_t walk_sidestep_primary_dir;
static int walk_sidestep_idx = 0;
static int walk_sidestep_ticks = 0;

/* ------------------------------------------------------------------ */
/* Task 10 part A: mask-accurate sub-tile pathing.
 *
 * The tile-level BFS above (kept for exit resolution and coarse
 * reachability -- see walk_grid_walkable/isexit) treats every nonzero
 * tile as a single all-or-nothing walkable cell. That's tile-blind: a
 * tile that's mostly floor but partly covered by furniture (a bed, a
 * table corner) still counts as fully walkable, so the BFS routes the
 * prisoner straight at it and only the sidestep/corner-rounding recovery
 * machinery discovers the obstruction -- costing real recovery time
 * (exactly the "pixel-pockets" weakness this task exists to fix).
 *
 * mask_footprint_blocked() below mirrors check_footprint()'s own
 * mask-sampling math (game.c:2170-2361, via get_tile_props() at
 * game.c:2114-2164) to test actual wall/furniture geometry at a given
 * room-pixel position, entirely read-only and using only local state (see
 * its own header comment for why it never touches game.c's tile_x/
 * tile_y/mask_offset/exit_offset/exit_dx/tunexit_tool globals). Sampling
 * this at the center of every 8x8px cell (4x4 cells per 32x32 tile --
 * SUB_PER_TILE below) builds walk_sub_walkable[], a finer floor grid the
 * subcell BFS (walk_bfs_sub) paths over instead of the coarse tile grid,
 * so a route can thread through a pixel-pocket the old tile-level BFS
 * could only stumble into and then recover from.
 *
 * Exit tiles keep working exactly as before: mask_footprint_blocked()
 * itself already treats an exit-mask overlap as passable (mirroring
 * check_footprint's own exit_mask collision test, minus the lock/grade
 * read fair play forbids -- see its header comment), and
 * walk_snapshot_grid() additionally forces every subcell of a tile the
 * coarse scan already flagged as an exit cell (readexit()&0x1F) to stay
 * walkable regardless of the mask probe, as a safety net matching the
 * existing tile-level convention (never worse than before this task).
 * (SUB_PER_TILE/SUB_SIZE/WALK_SUB_MAX_* are defined earlier, alongside
 * WALK_MAX_CELLS, so walk_path_x/y above could be sized off them.) */

static bool walk_sub_walkable[WALK_SUB_MAX_CELLS];
static uint16_t walk_sub_width = 0, walk_sub_height = 0;

/* Per-tile-id memoized mask/exit table lookup (mirrors get_tile_props,
 * game.c:2114-2164, minus the tunexit_tool/exit_dx bookkeeping our probe
 * never needs -- see mask_footprint_blocked's header comment). A subcell
 * grid probes the same handful of distinct tile ids thousands of times
 * (16 subcells/tile, and rooms reuse a small palette of tile graphics);
 * caching each id's resolved offsets the first time it's seen turns the
 * whole-room subgrid build from O(subcells * (NB_EXITS+NB_TUNNEL_EXITS))
 * into O(distinct_ids * (NB_EXITS+NB_TUNNEL_EXITS)) -- see
 * walk_snapshot_grid's timing note for the measured effect. Keyed by the
 * raw tile value (post TUNNEL_TILE_ADDON, so tunnel/non-tunnel entries
 * never collide); reset per grid snapshot rather than proven invalidation-
 * safe, since tile graphics don't change mid-walk and a fresh snapshot is
 * simple and always correct. */
#define MASK_TILE_CACHE_SIZE 1024
static bool mask_cache_valid[MASK_TILE_CACHE_SIZE];
static uint32_t mask_cache_mask_offset[MASK_TILE_CACHE_SIZE];
static uint32_t mask_cache_exit_offset[MASK_TILE_CACHE_SIZE];

static void mask_cache_reset(void)
{
    memset(mask_cache_valid, 0, sizeof(mask_cache_valid));
}

static void mask_get_tile_props(uint32_t tile, uint32_t* out_mask_offset,
                                 uint32_t* out_exit_offset)
{
    uint8_t u;
    uint32_t mask_offset, exit_offset;

    if (tile < MASK_TILE_CACHE_SIZE && mask_cache_valid[tile]) {
        *out_mask_offset = mask_cache_mask_offset[tile];
        *out_exit_offset = mask_cache_exit_offset[tile];
        return;
    }

    exit_offset = MASK_EMPTY;
    for (u = 0; u < NB_EXITS; u++) {
        if (readword((uint8_t*)fbuffer[LOADER], EXIT_TILES_LIST + 2*u) == tile) {
            exit_offset = EXIT_MASKS_START +
                readword((uint8_t*)fbuffer[LOADER], EXIT_MASKS_OFFSETS + 2*u);
            break;
        }
    }
    for (u = 0; u < NB_TUNNEL_EXITS; u++) {
        if (readword((uint8_t*)fbuffer[LOADER], TUNNEL_EXIT_TILES_LIST + 2*u) == tile)
            break;
    }
    if (u < IN_TUNNEL_EXITS_START)
        mask_offset = MASK_FULL;
    else
        mask_offset = TILE_MASKS_START +
            readlong((uint8_t*)fbuffer[LOADER], TILE_MASKS_OFFSETS + (tile<<2));

    if (tile < MASK_TILE_CACHE_SIZE) {
        mask_cache_valid[tile] = true;
        mask_cache_mask_offset[tile] = mask_offset;
        mask_cache_exit_offset[tile] = exit_offset;
    }
    *out_mask_offset = mask_offset;
    *out_exit_offset = exit_offset;
}

/* Read-only mirror of check_footprint()'s mask-sampling loop
 * (game.c:2170-2361): does a footprint centered at room-pixel (px,p2y)
 * [p2y doubled-Y, same convention as guybrush[].p2y] collide with wall
 * geometry? `tunnel` mirrors in_tunnel. Exit-mask overlaps are always
 * treated as open -- collision(footprint,exit_mask) true means "this wall
 * hit is actually a doorway", which check_footprint would resolve by
 * reading exit_flags (locked/grade); we never do that (fair play), so we
 * take the same stance the existing tile-level readexit()&0x1F test
 * already takes: an exit cell is walkable, full stop. Caller must already
 * have called set_room_xy() for the room being probed (room_x/room_y/
 * offset live) -- same precondition walk_snapshot_grid's own caller
 * context establishes; this function never calls it itself so a whole
 * subgrid build doesn't redundantly re-resolve room_x/room_y per cell. */
static bool mask_footprint_blocked(int16_t px, int16_t p2y, bool tunnel)
{
    uint32_t mask_offset[4], exit_offset[4];
    int16_t tile_x, tile_y;
    uint32_t footprint = tunnel ? TUNNEL_FOOTPRINT : SPRITE_FOOTPRINT;
    uint16_t mask_y;
    uint8_t i, u;
    int room_px_limit, room_py_limit;

    if (tunnel) px = (int16_t)(px - 16);
    p2y = (int16_t)(p2y - 1);

    tile_y = (int16_t)(p2y / 32);
    tile_x = (int16_t)(px / 32);

    room_px_limit = 32 * (int)room_x - 6;
    room_py_limit = 32 * (int)room_y - 6;
    if (px < 0 || p2y < 0 || px >= room_px_limit || p2y >= room_py_limit)
        return true;

    for (i = 0; i < 2; i++) {
        uint32_t tile = readtile(tile_x, tile_y) +
            (tunnel ? (uint32_t)TUNNEL_TILE_ADDON : 0);
        mask_get_tile_props(tile, &mask_offset[2*i], &exit_offset[2*i]);
        if ((px & 0x1F) < 16) {
            mask_offset[2*i+1] = mask_offset[2*i] + 2;
            exit_offset[2*i+1] = exit_offset[2*i] + 2;
        } else {
            mask_offset[2*i] += 2;
            exit_offset[2*i] += 2;
            if ((int)(tile_x+1) < (int)room_x) {
                uint32_t tile2 = readtile(tile_x+1, tile_y) +
                    (tunnel ? (uint32_t)TUNNEL_TILE_ADDON : 0);
                mask_get_tile_props(tile2, &mask_offset[2*i+1], &exit_offset[2*i+1]);
            } else {
                exit_offset[2*i+1] = MASK_EMPTY;
                mask_offset[2*i+1] = MASK_EMPTY;
            }
        }
        tile_y++;
    }

    mask_y = (uint16_t)((p2y & 0x1E) << 1);
    mask_offset[0] = (uint32_t)(mask_offset[0] + mask_y);
    mask_offset[1] = (uint32_t)(mask_offset[1] + mask_y);
    exit_offset[0] = (uint32_t)(exit_offset[0] + mask_y);
    exit_offset[1] = (uint32_t)(exit_offset[1] + mask_y);

    footprint >>= (px & 0x0F);

    for (i = 0; i < FOOTPRINT_HEIGHT; i++) {
        uint32_t tile_mask = to_long(
            readword((uint8_t*)fbuffer[LOADER], mask_offset[0]),
            readword((uint8_t*)fbuffer[LOADER], mask_offset[1]));
        uint32_t exit_mask = to_long(
            readword((uint8_t*)fbuffer[LOADER], exit_offset[0]),
            readword((uint8_t*)fbuffer[LOADER], exit_offset[1]));

        if (inverted_collision(footprint, tile_mask))
            return !collision(footprint, exit_mask);

        mask_y += 4;
        for (u = 0; u < 2; u++) {
            if (mask_y == 0x40) {
                mask_offset[u] = mask_offset[u+2];
                exit_offset[u] = exit_offset[u+2];
            } else {
                mask_offset[u] += 4;
                exit_offset[u] += 4;
            }
        }
    }
    return false;
}

/* Picks the subcell of tile (tile_x,tile_y) nearest to bias point
 * (bias_px,bias_p2y) among that tile's SUB_PER_TILE^2 subcells that
 * walk_sub_walkable[] actually marks walkable -- used to turn a tile-level
 * target (an exit tile, a plain {"tile":[x,y]} request, or an item's own
 * tile) into a concrete subcell BFS target biased toward the doorway/tile
 * center or (for item walks) the exact pickup anchor, so the subsequent
 * ITEM_PIXEL/CROSS phase starts as close as possible. Falls back to the
 * tile's own center-ish subcell (index (1,1) of 0..SUB_PER_TILE-1) if
 * NONE of the tile's subcells are walkable -- so this always returns some
 * subcell rather than failing; the subcell BFS itself is the authority on
 * whether that subcell is actually reachable. */
static void walk_resolve_target_subcell(int16_t tile_x, int16_t tile_y,
                                         int16_t bias_px, int16_t bias_p2y,
                                         int16_t* out_sx, int16_t* out_sy)
{
    int si, sj, best_si = 1, best_sj = 1;   /* sane default for SUB_PER_TILE==4 */
    long best_d = -1;
    bool found = false;

    for (sj = 0; sj < SUB_PER_TILE; sj++) {
        for (si = 0; si < SUB_PER_TILE; si++) {
            int sub_x = tile_x*SUB_PER_TILE + si;
            int sub_y = tile_y*SUB_PER_TILE + sj;
            int idx = sub_y*(int)walk_sub_width + sub_x;
            long dpx, dpy, d;
            if (idx < 0 || idx >= WALK_SUB_MAX_CELLS || !walk_sub_walkable[idx])
                continue;
            dpx = (tile_x*32 + si*SUB_SIZE + SUB_SIZE/2) - bias_px;
            dpy = (tile_y*32 + sj*SUB_SIZE + SUB_SIZE/2) - bias_p2y;
            d = dpx*dpx + dpy*dpy;
            if (!found || d < best_d) { found = true; best_d = d; best_si = si; best_sj = sj; }
        }
    }
    *out_sx = (int16_t)(tile_x*SUB_PER_TILE + best_si);
    *out_sy = (int16_t)(tile_y*SUB_PER_TILE + best_sj);
}

/* Clamps the current prisoner's exact pixel position down to a subcell
 * coordinate within the current walk_sub_width/height snapshot -- the
 * subcell-BFS analogue of the tile-level sx/sy clamp every handle_walk/
 * walk_sidestep_finish/walk_item_try_next_side call site used to do
 * inline; factored out once all three need it at subcell resolution. */
static void walk_sub_clamp_start(int16_t* out_sx, int16_t* out_sy)
{
    int16_t sx = (int16_t)(guybrush[current_nation].px / SUB_SIZE);
    int16_t sy = (int16_t)(guybrush[current_nation].p2y / SUB_SIZE);
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int16_t)walk_sub_width)  sx = (int16_t)walk_sub_width - 1;
    if (sy >= (int16_t)walk_sub_height) sy = (int16_t)walk_sub_height - 1;
    *out_sx = sx; *out_sy = sy;
}

/* Task 10 part B: classifies WHY the walk just stalled, at the moment a
 * stall is about to become a "blocked" result (final-waypoint stall with
 * no recovery left, sidestep-recovery re-path itself failing, corner-
 * rounding exhausted with no more approach sides). Order matters: door
 * (exit-tile-specific geometry) is checked before guard (a guard standing
 * in a doorway is still, first and foremost, a door the walk couldn't get
 * through) is checked before the generic static fallback. guard/door are
 * on-screen-visible facts (same fair-play tier as /room and /state's own
 * per-prisoner positions) -- this never reads exit lock/grade state. */
#define WALK_BLOCKED_GUARD_RADIUS 24
static walk_blocked_reason_t walk_classify_stall(void)
{
    int16_t my_px = guybrush[current_nation].px;
    int16_t my_p2y = guybrush[current_nation].p2y;
    uint16_t my_room = guybrush[current_nation].room;
    int i;

    if (walk_is_exit_target) {
        int16_t dtx = (int16_t)(my_px / 32), dty = (int16_t)(my_p2y / 32);
        int dist = abs((int)dtx - (int)walk_target_x) + abs((int)dty - (int)walk_target_y);
        /* Adjacency (Manhattan tile distance <=1), not just an exact
         * match -- a locked/closed door stall can happen one tile short
         * of the doorway (the approach itself stalls before ever reaching
         * the exact exit tile; matches the same adjacency threshold
         * walk_pump's stall handler uses to hand off to WALK_PHASE_CROSS,
         * see its own comment) just as readily as a stall exactly on it. */
        if (dist <= 1)
            return WALK_BLOCK_DOOR;
    }

    for (i = 0; i < NB_GUYBRUSHES; i++) {
        int16_t dpx, dp2y;
        if (i == current_nation) continue;
        if (guybrush[i].room != my_room) continue;
        dpx  = (int16_t)(guybrush[i].px  - my_px);
        dp2y = (int16_t)(guybrush[i].p2y - my_p2y);
        if (dpx < 0) dpx = (int16_t)-dpx;
        if (dp2y < 0) dp2y = (int16_t)-dp2y;
        if (dpx <= WALK_BLOCKED_GUARD_RADIUS && dp2y <= WALK_BLOCKED_GUARD_RADIUS)
            return WALK_BLOCK_GUARD;
    }

    return WALK_BLOCK_STATIC;
}

/* Single choke point pairing every walk_cancel(WALK_BLOCKED) call with the
 * reason that produced it -- mirrors walk_cancel() itself being the single
 * choke point for key release. */
static void walk_set_blocked(walk_blocked_reason_t reason)
{
    walk_blocked_reason = reason;
    walk_cancel(WALK_BLOCKED);
}

enum { WALK_SNAP_OK = 0, WALK_SNAP_NO_ROOM, WALK_SNAP_BAD_EXIT };

/* Snapshots the CURRENT room's walkable floor into walk_grid_walkable[]
 * (identical fair-play floor test to /room: any nonzero tile id, via the
 * same readtile() macro, OR'd with an exit cell (readexit(x,y) & 0x1F)
 * so exit tiles are walkable even over a void (id 0) tile -- mirroring
 * /room's grid, which marks 'E' over void cells the same way. Only the
 * exit index's low 5 bits' nonzero-ness is read, never door/exit status
 * such as locked/grade), and optionally resolves the exit_index'th exit
 * cell (in the same y-then-x scan order /room's own exits array uses)
 * into (*exit_x,*exit_y) when exit_index>=0.
 * Mirrors handle_room's save/restore-around-set_room_xy pattern exactly
 * (see its comment) -- room_x/room_y/offset are always restored before
 * returning, on every path, so this never disturbs engine bookkeeping the
 * rest of the frame depends on. */
static int walk_snapshot_grid(uint16_t* out_width, uint16_t* out_height,
                               int exit_index, int16_t* exit_x, int16_t* exit_y)
{
    uint16_t room = guybrush[current_nation].room;
    bool outside = (room == ROOM_OUTSIDE);
    uint16_t saved_room_x = room_x, saved_room_y = room_y;
    uint32_t saved_offset = offset;
    uint16_t width, height;
    int x, y, seen = 0;
    bool found_exit = (exit_index < 0);

    if (!outside) {
        uint32_t raw = readlong((uint8_t*)fbuffer[ROOMS],
                                CRM_OFFSETS_START + 4*(uint32_t)room);
        if (raw == 0xFFFFFFFF) return WALK_SNAP_NO_ROOM;
    }

    set_room_xy(room);
    width = room_x;
    height = room_y;

    if (width == 0 || height == 0 || width > WALK_MAX_W || height > WALK_MAX_H) {
        room_x = saved_room_x; room_y = saved_room_y; offset = saved_offset;
        return WALK_SNAP_NO_ROOM;
    }

    for (y = 0; y < (int)height; y++) {
        for (x = 0; x < (int)width; x++) {
            bool is_exit = (readexit(x, y) & 0x1F) != 0;
            walk_grid_walkable[y*(int)width + x] = (readtile(x, y) != 0) || is_exit;
            walk_grid_isexit[y*(int)width + x] = is_exit;
            if (exit_index >= 0 && !found_exit && is_exit) {
                if (seen == exit_index) {
                    *exit_x = (int16_t)x; *exit_y = (int16_t)y;
                    found_exit = true;
                }
                seen++;
            }
        }
    }

    /* Task 10 part A: mask-accurate subcell grid, built in the same
     * room_x/room_y/offset-live window as the tile scan above (mask
     * lookups need room_x for the same "does the neighbor tile exist"
     * bound check check_footprint's own get_tile_props does). Timed via
     * mtime() and reported through printb (opt_debug-gated, so this is a
     * zero-cost no-op in a normal run) -- see the brief's "VERIFY timing"
     * requirement; worst case is the full outside map, 84*72 tiles * 16
     * subcells = ~96.8k mask probes. */
    {
        bool tunnel = (guybrush[current_nation].state & STATE_TUNNELING) != 0;
        uint16_t sub_w = (uint16_t)(width * SUB_PER_TILE);
        uint16_t sub_h = (uint16_t)(height * SUB_PER_TILE);
        int tx, ty, si, sj;
        uint64_t t0 = mtime();

        mask_cache_reset();
        for (ty = 0; ty < (int)height; ty++) {
            for (tx = 0; tx < (int)width; tx++) {
                bool tile_is_exit = walk_grid_isexit[ty*(int)width + tx];
                for (sj = 0; sj < SUB_PER_TILE; sj++) {
                    for (si = 0; si < SUB_PER_TILE; si++) {
                        int16_t cx = (int16_t)(tx*32 + si*SUB_SIZE + SUB_SIZE/2);
                        int16_t cy = (int16_t)(ty*32 + sj*SUB_SIZE + SUB_SIZE/2);
                        int sub_idx = (ty*SUB_PER_TILE+sj)*(int)sub_w +
                                      (tx*SUB_PER_TILE+si);
                        bool blocked = mask_footprint_blocked(cx, cy, tunnel);
                        walk_sub_walkable[sub_idx] = (!blocked) || tile_is_exit;
                    }
                }
            }
        }
        walk_sub_width = sub_w;
        walk_sub_height = sub_h;
        printb("walk: subgrid %ux%u (%d cells, tunnel=%d) built in %llums\n",
               sub_w, sub_h, (int)sub_w*(int)sub_h, (int)tunnel,
               (unsigned long long)(mtime() - t0));
    }

    room_x = saved_room_x; room_y = saved_room_y; offset = saved_offset;
    *out_width = width; *out_height = height;
    return found_exit ? WALK_SNAP_OK : WALK_SNAP_BAD_EXIT;
}

/* Converts a raw subcell path (walk_path_x/y[0..len-1], as reconstructed
 * by walk_bfs_sub from its BFS parent-pointer walk) into a deduplicated
 * TILE path, in place -- the design walk_bfs_sub actually ships with; see
 * its own header comment for why PATH-phase steering targets tiles
 * (32-unit, WALK_DEADBAND) rather than raw subcells despite the BFS itself
 * running at subcell (mask-accurate) resolution. `start_sub_x/y` is the
 * BFS's own start subcell (not itself stored in the path array). Every
 * kept tile is exactly one step (in tile-space) from the previous kept
 * tile: a 4-connected subcell step can cross at most one tile boundary,
 * so two DIFFERENT consecutive tiles in the dedup'd output are always
 * adjacent -- this is what still lets walk_exit_dir_candidates treat
 * consecutive path entries as a reliable direction-of-arrival signal. The
 * output cursor `out` never exceeds the read cursor `k`, so writing
 * behind it (in place, into the same walk_path_x/y arrays the raw subcell
 * path was just read from) is safe. Returns the number of tiles written
 * (0 for a zero-length subcell path, i.e. start and target were already
 * the same subcell -- their shared tile isn't itself emitted, matching
 * the pre-existing "target tile == start tile" convention every caller
 * already relies on for a zero-length walk). */
static int walk_path_to_tiles(int16_t start_sub_x, int16_t start_sub_y, int len)
{
    int out = 0, k;
    int16_t prev_tx = (int16_t)(start_sub_x / SUB_PER_TILE);
    int16_t prev_ty = (int16_t)(start_sub_y / SUB_PER_TILE);

    for (k = 0; k < len; k++) {
        int16_t tx = (int16_t)(walk_path_x[k] / SUB_PER_TILE);
        int16_t ty = (int16_t)(walk_path_y[k] / SUB_PER_TILE);
        if (tx != prev_tx || ty != prev_ty) {
            walk_path_x[out] = tx;
            walk_path_y[out] = ty;
            out++;
            prev_tx = tx; prev_ty = ty;
        }
    }
    return out;
}

/* 4-connected BFS from subcell (sx,sy) to (tx,ty) over walk_sub_walkable[]
 * (already populated by walk_snapshot_grid, sub_w x sub_h -- see the
 * "mask-accurate sub-tile pathing" block above): the REACHABILITY test is
 * mask-accurate subcell resolution, so a tile that's only PARTIALLY open
 * (furniture covering most, but not all, of it) is routed around/through
 * correctly instead of the coarse tile-level BFS's blind "any nonzero
 * pixel counts" test -- this is what actually fixes "BFS is tile-blind".
 * On success, fills walk_path_x/y[0..*out_len-1] with a DEDUPLICATED TILE
 * path (walk_path_to_tiles above), strictly after the start's tile through
 * the target's tile inclusive, and returns true.
 *
 * DESIGN NOTE (why waypoints are tiles, not raw subcells -- see the Task
 * 10 report's self-review for the full write-up): an earlier version of
 * this function returned smoothed SUBCELL waypoints directly, and drove
 * WALK_PHASE_PATH's steering at that same resolution. Verified live during
 * this task that this is NOT reliably steerable: PATH-phase's simultaneous
 * 2-axis deadband steering, and the stall-recovery amplitudes (SIDESTEP's
 * fixed-tick perpendicular probes, ITEM_PIXEL's 12/24px corner-rounding),
 * are all tuned to TILE scale (32 units) and become unreliable at subcell
 * scale (4-8 units) in tight/diagonal room geometry -- concretely, room
 * 251's own floor shape (a diagonal wall, verified via the mask probe
 * itself, not a guess) repeatedly stalled a pure-subcell walk in a way
 * neither the sidestep nor the one-shot re-path recovery could reliably
 * escape, even though the SAME underlying tiles, driven at tile
 * resolution with the SAME recovery machinery, are what this exact walk
 * already passed against before Task 10. Resampling the mask-accurate
 * subcell path down to its distinct tiles keeps the reachability
 * improvement (a tile with zero walkable subcells is now correctly
 * excluded, and a tile only reachable via a specific corner is still
 * found) while driving the actual walk with the SAME steering/recovery
 * design that was already proven reliable -- the smoothing this now does
 * is at tile granularity, same as the old tile-level walk_bfs effectively
 * needed no separate smoothing step for (consecutive tile-level BFS steps
 * are never collinear-mergeable beyond what tile adjacency already is).
 *
 * Same overall structure as the retired tile-level walk_bfs, but cell
 * indices are int32_t throughout during the subcell search (a subcell
 * grid can exceed WALK_SUB_MAX_CELLS ~387k, well past int16_t's +-32767
 * range, unlike the tile grid's <=6048 cells) -- only the FINAL x/y
 * coordinates written into walk_path_x/y (tile indices, tiny) are
 * int16_t. */
static bool walk_bfs_sub(uint16_t sub_w, uint16_t sub_h,
                          int16_t sx, int16_t sy, int16_t tx, int16_t ty,
                          int* out_len)
{
    static int32_t prev[WALK_SUB_MAX_CELLS];
    static int32_t queue[WALK_SUB_MAX_CELLS];
    static bool visited[WALK_SUB_MAX_CELLS];
    static int32_t rev[WALK_SUB_MAX_CELLS];
    static const int8_t dxs[4] = { 1, -1, 0, 0 };
    static const int8_t dys[4] = { 0, 0, 1, -1 };
    int32_t qh = 0, qt = 0, i;
    int32_t start = (int32_t)sy*(int32_t)sub_w + sx;
    int32_t target = (int32_t)ty*(int32_t)sub_w + tx;
    uint64_t t0 = mtime();

    /* The START cell is never rejected on walkability, only the target is:
     * the prisoner is, by construction, ALREADY standing somewhere inside
     * it (real check_footprint calls already validated their exact pixel
     * position every tick they walked there) -- but a single mask sample
     * at the cell's geometric CENTER can legitimately disagree with a
     * pixel a few units away within that same cell (the footprint's own
     * ~4px real-pixel height is comparable to the 8-unit subcell size, so
     * near an edge, "is the center open" and "is the actual standing spot
     * open" can differ). Treating the start as reachable regardless keeps
     * that sampling granularity from ever manufacturing a spurious "no
     * path" purely because of where, within its cell, the walk happens to
     * begin -- BFS expansion from it still requires each subsequent
     * neighbor cell to test walkable, so this doesn't relax anything about
     * the route itself, only the (trivially true) fact that where the
     * prisoner already is counts as reachable. */
    if (!walk_sub_walkable[target])
        return false;

    memset(visited, 0, (size_t)sub_w * (size_t)sub_h * sizeof(bool));
    visited[start] = true;
    prev[start] = -1;
    queue[qt++] = start;

    while (qh < qt) {
        int32_t u = queue[qh++];
        int32_t ux = u % (int32_t)sub_w, uy = u / (int32_t)sub_w;
        if (u == target) break;
        /* No edge-midpoint check between adjacent cell centers here (an
         * earlier version of this loop had one): tried at the original
         * 8-unit cell size to guard against a thin obstacle sitting
         * entirely in the gap between two sample points, but it rejected
         * genuinely-connected diagonal corridors instead -- a staircase-
         * shaped wall's orthogonal-neighbor midpoints can legitimately
         * sit ON the diagonal boundary even though both cells, and the
         * real deadband-corrected transit between them, are fine (see the
         * Task 10 report's self-review for the concrete case). Shrinking
         * SUB_SIZE to 4 units (below the engine's own ~4px vertical
         * mask-scan step, see mask_footprint_blocked's header comment)
         * fixes the gap-aliasing problem at its root -- the sample
         * spacing itself -- making a per-edge check both unnecessary and
         * (for diagonal geometry) actively harmful. */
        for (i = 0; i < 4; i++) {
            int32_t nx = ux + dxs[i], ny = uy + dys[i], v;
            if (nx < 0 || ny < 0 || nx >= (int32_t)sub_w || ny >= (int32_t)sub_h)
                continue;
            v = ny*(int32_t)sub_w + nx;
            if (!walk_sub_walkable[v] || visited[v]) continue;
            visited[v] = true;
            prev[v] = u;
            queue[qt++] = v;
        }
    }

    if (!visited[target])
        return false;

    {
        int len = 0;
        int32_t cur = target;
        while (cur != start) {
            rev[len++] = cur;
            cur = prev[cur];
        }
        for (i = 0; i < len; i++) {
            int32_t cell = rev[len-1-i];
            walk_path_x[i] = (int16_t)(cell % (int32_t)sub_w);
            walk_path_y[i] = (int16_t)(cell / (int32_t)sub_w);
        }
        *out_len = walk_path_to_tiles(sx, sy, len);
        printb("walk: bfs_sub %ux%u raw_sub_len=%d tile_len=%d took %llums\n",
               sub_w, sub_h, len, *out_len,
               (unsigned long long)(mtime() - t0));
    }
    return true;
}

/* Minimal JSON int-pair extractor for "field":[a,b] (fixed tiny schema, no
 * lib -- same spirit as json_str/json_int above). */
static bool json_intpair(const char* body, const char* field, long* a, long* b)
{
    char pat[32]; const char* p;
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    p = strstr(body, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':'); if (!p) return false;
    p = strchr(p, '[');               if (!p) return false;
    p++;
    *a = strtol(p, (char**)&p, 10);
    p = strchr(p, ',');               if (!p) return false;
    p++;
    *b = strtol(p, NULL, 10);
    return true;
}

/* ---- Task 9 direction helpers (used by CROSSING and SIDESTEP phases) --- */

static uint8_t walk_dir_key(walk_dir_t d)
{
    switch (d) {
        case WALK_DIR_UP:   return walk_key_up;
        case WALK_DIR_DOWN: return walk_key_down;
        case WALK_DIR_LEFT: return walk_key_left;
        default:            return walk_key_right;   /* WALK_DIR_RIGHT */
    }
}

/* Releases all four walk-held direction keys except `code`, then presses
 * `code` -- the single-key-at-a-time hold used by both the CROSSING (one
 * outward direction) and SIDESTEP (one perpendicular direction) phases,
 * as opposed to PATH-phase steering below, which can hold two axes at
 * once for diagonal motion. */
static void walk_hold_only(uint8_t code)
{
    uint8_t keys[4] = { walk_key_up, walk_key_down, walk_key_left, walk_key_right };
    int i;
    for (i = 0; i < 4; i++) {
        if (keys[i] == code) {
            key_down[keys[i]] = true;
        } else {
            key_down[keys[i]] = false;
            key_readonce[keys[i]] = false;
        }
    }
}

/* Does direction `d`, taken from exit tile (ex,ey), lead off the current
 * room's grid or into a non-walkable (void/wall) neighbor cell? This is
 * the brief's geometry-only outward-direction test: the doorway's
 * "outward" side is the one that does NOT lead to more walkable floor
 * within this room (walking further into the room is the opposite of
 * crossing the exit). Reads only walk_grid_walkable[] -- the same
 * fair-play floor snapshot /room's grid and the BFS already use; never
 * touches exit lock/grade state. */
static bool walk_dir_qualifies(int16_t ex, int16_t ey, walk_dir_t d)
{
    int16_t nx = ex, ny = ey;
    switch (d) {
        case WALK_DIR_UP:    ny--; break;
        case WALK_DIR_DOWN:  ny++; break;
        case WALK_DIR_LEFT:  nx--; break;
        case WALK_DIR_RIGHT: nx++; break;
    }
    if (nx < 0 || ny < 0 || nx >= (int16_t)walk_width || ny >= (int16_t)walk_height)
        return true;   /* off-grid: edge-of-map exit */
    return !walk_grid_walkable[(int)ny*(int)walk_width + nx];
}

/* Determines up to 2 candidate outward directions to hold during the
 * CROSSING phase for exit tile (ex,ey), writing them into out[0..return
 * value-1]. Ordering: the direction the BFS path actually arrived from is
 * tried first if it satisfies walk_dir_qualifies() -- continuing the same
 * way keeps walking straight through the doorway, since the corridor
 * leading to a door is, in every room layout this engine presents,
 * aligned with the door's own crossing axis. Remaining qualifying
 * directions (the brief's off-grid-or-non-walkable-neighbor test) fill
 * the rest, up to 2 total, in a fixed scan order. Falls back to the
 * arrival direction (or DOWN if there wasn't one, e.g. a zero-length
 * path) if nothing qualifies, rather than returning an empty list.
 *
 * IMPLEMENTER NOTE (per the brief -- study check_footprint first, prefer
 * engine geometry if it's cheap and fair): considered reusing the
 * engine's own exit_dx[] (game.c, get_tile_props/check_footprint,
 * ~game.c:2112-2260) for this instead. Not used: exit_dx is a
 * footprint-quadrant selector computed relative to one specific attempted
 * (dx,d2y) motion, mid-way through check_footprint's own 4-mask collision
 * scan -- not a standalone "which way does this door face" value. Reusing
 * it here would mean replicating check_footprint's whole mask-offset
 * machinery, right next to the exit_flags lock/grade reads fair play
 * forbids touching at all, just to recover information the walkable-floor
 * snapshot already gives us directly. The walkable-neighbor test below is
 * pure geometry (the same snapshot /room and the BFS already use) and no
 * more expensive than up to 4 array lookups. */
static int walk_exit_dir_candidates(int16_t ex, int16_t ey, walk_dir_t* out)
{
    static const walk_dir_t all_dirs[4] = {
        WALK_DIR_RIGHT, WALK_DIR_LEFT, WALK_DIR_DOWN, WALK_DIR_UP
    };
    int n = 0, i;
    bool have_arrival = false;
    walk_dir_t arrival = WALK_DIR_DOWN;

    if (walk_path_len >= 1) {
        int16_t fx, fy, tx, ty;
        if (walk_path_len >= 2) {
            fx = walk_path_x[walk_path_len - 2];
            fy = walk_path_y[walk_path_len - 2];
        } else {
            fx = walk_start_x;
            fy = walk_start_y;
        }
        tx = walk_path_x[walk_path_len - 1];
        ty = walk_path_y[walk_path_len - 1];
        /* walk_path_x/y are tile waypoints derived from a mask-accurate
         * SUBCELL search (walk_bfs_sub/walk_path_to_tiles) rather than a
         * raw tile-level BFS, but walk_path_to_tiles guarantees the same
         * invariant a raw tile BFS would: two DIFFERENT consecutive
         * entries are always exactly one tile apart (a 4-connected
         * subcell step can cross at most one tile boundary). The SIGN of
         * the delta (rather than an exact +-1 equality test) still
         * unambiguously gives the direction of travel on whichever axis
         * actually moved, and works either way. */
        if      (tx - fx >  0) { arrival = WALK_DIR_RIGHT; have_arrival = true; }
        else if (tx - fx <  0) { arrival = WALK_DIR_LEFT;  have_arrival = true; }
        else if (ty - fy >  0) { arrival = WALK_DIR_DOWN;  have_arrival = true; }
        else if (ty - fy <  0) { arrival = WALK_DIR_UP;    have_arrival = true; }
    }

    if (have_arrival && walk_dir_qualifies(ex, ey, arrival))
        out[n++] = arrival;

    for (i = 0; i < 4 && n < 2; i++) {
        if (n > 0 && all_dirs[i] == out[0]) continue;
        if (walk_dir_qualifies(ex, ey, all_dirs[i]))
            out[n++] = all_dirs[i];
    }

    if (n == 0)
        out[n++] = have_arrival ? arrival : WALK_DIR_DOWN;

    return n;
}

/* CROSSING phase (part A): pins the outward key for candidate direction
 * walk_cross_cand[idx] (held unconditionally -- never deadbanded, since
 * the whole point is to keep leaving) while deadband-correcting the
 * PERPENDICULAR axis toward the exit tile's center every tick, exactly
 * the way PATH-phase steering corrects both axes at once ("diagonals
 * allowed as in walk_pump", per the brief). This matters: a prisoner who
 * reached the exit tile slightly off-center on the cross axis, then had
 * only the outward key held with no correction, would keep pushing along
 * a line that never actually satisfies the doorway's collision mask and
 * stall forever even though the door itself is open -- caught live during
 * this task's own verification pass (every non-edge door in room 253
 * reported `blocked` until this fix; edge-of-map doors happened to work
 * anyway because they need no perpendicular alignment beyond what the
 * BFS's own tile-center-seeking PATH phase already provided on arrival).
 * On a leg timeout, advances to the next candidate (wrapping ends a
 * round); after WALK_CROSS_MAX_ROUNDS rounds with no room change, gives
 * up. Success (room changed) is detected by walk_pump's shared preamble,
 * not here. */
static void walk_pump_cross(void)
{
    int16_t px, p2y, cx, cy;
    walk_dir_t d;

    walk_cross_ticks++;
    if (walk_cross_ticks >= WALK_CROSS_LEG_TICKS) {
        walk_cross_ticks = 0;
        walk_cross_idx++;
        if (walk_cross_idx >= walk_cross_ncand) {
            walk_cross_idx = 0;
            walk_cross_round++;
            if (walk_cross_round >= WALK_CROSS_MAX_ROUNDS) {
                /* Every candidate outward direction exhausted its leg
                 * budget with no room change -- Task 10 part B: this is
                 * definitionally the CROSS phase giving up, so "door" per
                 * the brief regardless of what walk_classify_stall() would
                 * otherwise guess (a guard could ALSO be blocking the
                 * doorway, but the phase itself is the more specific,
                 * more useful signal here). */
                walk_set_blocked(WALK_BLOCK_DOOR);
                return;
            }
        }
    }

    d   = walk_cross_cand[walk_cross_idx];
    px  = guybrush[current_nation].px;
    p2y = guybrush[current_nation].p2y;
    /* Perpendicular-correction target: the mask-validated walkable
     * subcell (walk_target_sub_x/y) rather than the tile's raw geometric
     * center -- a doorway's real opening is not always centered in its
     * tile (see walk_target_sub_x/y's own header comment), and the plain
     * center can itself sit outside the actual passable column. Still not
     * a full fix for every possible off-center doorway (the mask-
     * validated point is the WALKABLE column nearest the tile's center,
     * not necessarily the doorway's own -- see the Task 10 report for a
     * documented residual case), but strictly better than the fixed tile
     * center for any doorway whose opening is off-center but still
     * reasonably close to it. */
    cx  = (int16_t)(walk_target_sub_x * SUB_SIZE + SUB_SIZE/2);
    cy  = (int16_t)(walk_target_sub_y * SUB_SIZE + SUB_SIZE/2);

    /* Pin the primary (outward) axis. */
    if (d == WALK_DIR_LEFT) {
        key_down[walk_key_left] = true;
        key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
    } else if (d == WALK_DIR_RIGHT) {
        key_down[walk_key_right] = true;
        key_down[walk_key_left] = false; key_readonce[walk_key_left] = false;
    } else if (d == WALK_DIR_UP) {
        key_down[walk_key_up] = true;
        key_down[walk_key_down] = false; key_readonce[walk_key_down] = false;
    } else {
        key_down[walk_key_down] = true;
        key_down[walk_key_up] = false; key_readonce[walk_key_up] = false;
    }

    /* Deadband-correct the perpendicular axis toward the exit tile's
     * center (same test PATH-phase steering uses). */
    if (d == WALK_DIR_LEFT || d == WALK_DIR_RIGHT) {
        if (p2y < cy - WALK_DEADBAND) {
            key_down[walk_key_down] = true;
            key_down[walk_key_up] = false; key_readonce[walk_key_up] = false;
        } else if (p2y > cy + WALK_DEADBAND) {
            key_down[walk_key_up] = true;
            key_down[walk_key_down] = false; key_readonce[walk_key_down] = false;
        } else {
            key_down[walk_key_up] = false; key_readonce[walk_key_up] = false;
            key_down[walk_key_down] = false; key_readonce[walk_key_down] = false;
        }
    } else {
        if (px < cx - WALK_DEADBAND) {
            key_down[walk_key_right] = true;
            key_down[walk_key_left] = false; key_readonce[walk_key_left] = false;
        } else if (px > cx + WALK_DEADBAND) {
            key_down[walk_key_left] = true;
            key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
        } else {
            key_down[walk_key_left] = false; key_readonce[walk_key_left] = false;
            key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
        }
    }
}

/* Shared tail of SIDESTEP recovery (also used by walk_pump_sidestep's
 * early-success path below): release keys, take a fresh grid snapshot,
 * re-BFS from wherever the prisoner now is to the walk's ORIGINAL target
 * (walk_target_x/y, not whatever waypoint we'd been chasing), and resume
 * WALK_PHASE_PATH. Blocked if the fresh snapshot or the re-BFS fails (e.g.
 * the sidestep itself ran into a wall and made no progress). */
static void walk_sidestep_finish(void)
{
    uint16_t width = 0, height = 0;
    int16_t ssx, ssy, stx, sty, bias_px, bias_p2y;
    int new_len = 0, snap;

    walk_release_keys();
    snap = walk_snapshot_grid(&width, &height, -1, NULL, NULL);
    if (snap != WALK_SNAP_OK) { walk_set_blocked(walk_classify_stall()); return; }
    walk_width = width;
    walk_height = height;

    if (walk_target_x < 0 || walk_target_y < 0 ||
        walk_target_x >= (int16_t)width || walk_target_y >= (int16_t)height) {
        walk_set_blocked(walk_classify_stall());
        return;
    }

    /* Bias toward the TILE's center even for an item walk, not the exact
     * item pickup anchor -- see walk_resolve_target_subcell's call sites
     * in handle_walk for why: PATH-phase (simple simultaneous 2-axis
     * deadband steering) only needs to get the prisoner into the general
     * vicinity of the target tile, exactly like a plain-tile walk. Final
     * pixel-precision approach into a tight furniture pocket is
     * WALK_PHASE_ITEM_PIXEL's job (dedicated single-axis steering +
     * bounded corner-rounding) -- biasing PATH-phase's own subcell target
     * toward the anchor was tried and found to route PATH-phase itself
     * through the same tight geometry ITEM_PIXEL exists to handle,
     * defeating that separation (see the Task 10 report's self-review). */
    walk_sub_clamp_start(&ssx, &ssy);
    bias_px = (int16_t)(walk_target_x*32 + 16);
    bias_p2y = (int16_t)(walk_target_y*32 + 16);
    walk_resolve_target_subcell(walk_target_x, walk_target_y, bias_px, bias_p2y,
                                 &stx, &sty);
    walk_target_sub_x = stx;
    walk_target_sub_y = sty;

    if (!walk_bfs_sub(walk_sub_width, walk_sub_height, ssx, ssy, stx, sty, &new_len)) {
        walk_set_blocked(walk_classify_stall());
        return;
    }

    walk_path_len = new_len;
    walk_path_idx = 0;
    walk_stall_ticks = 0;
    walk_stall_px = guybrush[current_nation].px;
    walk_stall_p2y = guybrush[current_nation].p2y;
    walk_phase = WALK_PHASE_PATH;
}

/* Minimum combined pixel displacement (from the position where the stall
 * -- and so this whole recovery -- began) that counts as genuine escape
 * rather than collision-response jitter (observed jitter tops out around
 * 4px even on a fully blocked axis; a real single-axis move covers well
 * over 10px in one WALK_SIDESTEP_LEG_TICKS leg). */
#define WALK_SIDESTEP_ESCAPE_PX 10

/* SIDESTEP (recovery, part B) phase: hold one perpendicular side, then
 * retry the ORIGINAL blocked primary direction alone, then the OTHER
 * perpendicular side, then retry primary again -- each for
 * WALK_SIDESTEP_LEG_TICKS ticks. A primary-direction retry that actually
 * moves the prisoner a real distance (not just jitter) ends recovery
 * immediately via walk_sidestep_finish(); this is deliberately checked
 * after EACH perpendicular leg rather than only once at the end, because
 * the two perpendicular legs go opposite ways and so, left to run both
 * before ever trying primary, would cancel each other back out to
 * approximately the starting position -- exactly what a straight PATH-phase
 * stall trying to squeeze past a furniture corner needs: PATH-phase's
 * ordinary steering holds both axes at once, which the engine rejects as a
 * single atomic move if EITHER axis would collide, even when each axis
 * individually is free (verified live: room 251's post-item-pickup corner
 * lets a single held direction cross a full tile boundary and then a
 * single held perpendicular direction clear the furniture entirely, while
 * holding both together never moves the prisoner at all). Once all four
 * legs are exhausted with no real progress, falls through to
 * walk_sidestep_finish() exactly as the two-leg version did. */
static void walk_pump_sidestep(void)
{
    int16_t px, p2y, dpx, dp2y;

    if (++walk_sidestep_ticks < WALK_SIDESTEP_LEG_TICKS)
        return;

    px = guybrush[current_nation].px;
    p2y = guybrush[current_nation].p2y;

    if (walk_sidestep_idx == 1 || walk_sidestep_idx == 3) {
        dpx  = (int16_t)(px  - walk_stall_px);
        dp2y = (int16_t)((p2y - walk_stall_p2y) / 2);
        if ((dpx < 0 ? -dpx : dpx) + (dp2y < 0 ? -dp2y : dp2y) >= WALK_SIDESTEP_ESCAPE_PX) {
            walk_sidestep_finish();
            return;
        }
    }

    walk_sidestep_idx++;
    if (walk_sidestep_idx < 4) {
        walk_sidestep_ticks = 0;
        /* Legs 0,2: perpendicular probe. Legs 1,3: retry the original
         * blocked primary direction alone. */
        if (walk_sidestep_idx == 2)
            walk_hold_only(walk_dir_key(walk_sidestep_dir[1]));
        else
            walk_hold_only(walk_dir_key(walk_sidestep_primary_dir));
        return;
    }

    walk_sidestep_finish();
}

/* ITEM_PIXEL phase (the /walk item+pickup feature): entered once the BFS
 * path for an item walk reaches the prop's tile (or its nearest walkable
 * neighbor, see handle_walk). The BFS/PATH machinery only gets the
 * prisoner to a *tile*; the engine's actual pickup trigger is a pixel-
 * precision window around the prop's exact anchor (walk_item_anchor_x/y,
 * see its own comment above) that can sit off-center within that tile, or
 * be partly obstructed by furniture at pixel level -- tile precision alone
 * is not enough, hence this dedicated phase.
 *
 * Per-axis independent steering (brief, verbatim): a straight-line
 * (both-axes-at-once) approach, like PATH-phase's diagonal steering, can
 * stall forever against a furniture corner that a single-axis approach
 * would walk around. So this phase steers ONE axis at a time -- x first
 * (holding only a left/right key, y keys released) until x settles within
 * +-WALK_ITEM_DEADBAND of the anchor, THEN y (holding only up/down, x
 * keys released) -- rather than holding all four/two keys toward the
 * anchor's exact center the way PATH-phase does. "Arrived" requires BOTH
 * axes within the looser +-WALK_ITEM_MARGIN at once, matching the engine's
 * pickup window.
 *
 * See the walk_item_round* state block above for the corner-rounding
 * stall-recovery design (a bounded, MEASURED sidestep-then-reattempt
 * sequence) and why it replaced an earlier tick-count/exact-equality
 * zigzag that got stuck retrying the wrong side forever. */
static void walk_pump_item_round(int16_t px, int16_t p2y,
                                  int16_t dx, int16_t dy,
                                  int16_t adx, int16_t ady);

static void walk_item_start_round(int16_t px, int16_t p2y, bool primary_is_x)
{
    /* First sidestep goes TOWARD the anchor's perpendicular offset (the
     * free path around an obstacle's corner is almost always on the side
     * the target is on); later rounds alternate away from it. Side 0 =
     * positive direction (down/right), side 1 = negative (up/left). */
    int16_t perp_delta = primary_is_x ? (int16_t)(p2y/2 - walk_item_anchor_y)
                                      : (int16_t)(px - walk_item_anchor_x);
    int toward = (perp_delta < 0) ? 0 : 1;   /* anchor below/right of us -> side 0 */
    walk_item_rounding = true;
    walk_item_round_sidestepping = true;
    walk_item_round_side = ((walk_item_round & 1) == 0) ? toward : (1 - toward);
    walk_item_round_primary_is_x = primary_is_x;
    walk_item_round_ref_px = px;
    walk_item_round_ref_p2y = p2y;
    walk_item_round_ticks = 0;
    walk_item_round++;
}

/* Approach-side cycling (see walk_item_prop_tx block above): pick the next
 * untried walkable 4-neighbor of the prop tile, BFS to it, and resume the
 * PATH phase (arrival there re-enters ITEM_PIXEL as usual since
 * walk_is_item_target stays set). Returns false when no untried side
 * remains or nothing is reachable. */
static bool walk_item_try_next_side(void)
{
    static const int8_t adx[4] = { 1, -1, 0, 0 };   /* E, W, S, N */
    static const int8_t ady[4] = { 0, 0, 1, -1 };
    uint16_t width = 0, height = 0;
    int16_t ssx, ssy;
    int i, new_len = 0;

    if (walk_item_prop_tx < 0 ||
        walk_snapshot_grid(&width, &height, -1, NULL, NULL) != WALK_SNAP_OK)
        return false;
    walk_width = width;
    walk_height = height;
    walk_sub_clamp_start(&ssx, &ssy);

    for (i = 0; i < 4; i++) {
        int16_t nx = (int16_t)(walk_item_prop_tx + adx[i]);
        int16_t ny = (int16_t)(walk_item_prop_ty + ady[i]);
        int16_t stx, sty;
        if (walk_item_tried_mask & (uint8_t)(1 << i))
            continue;
        walk_item_tried_mask |= (uint8_t)(1 << i);
        if (nx < 0 || ny < 0 || nx >= (int16_t)width || ny >= (int16_t)height)
            continue;
        if (!walk_grid_walkable[(int)ny*(int)width + nx])
            continue;
        /* Tile-center bias, not the item anchor -- see handle_walk's
         * matching comment: PATH-phase only needs to reach the general
         * vicinity of this approach tile, not the precise pickup pixel. */
        walk_resolve_target_subcell(nx, ny, (int16_t)(nx*32 + 16),
                                     (int16_t)(ny*32 + 16), &stx, &sty);
        if (!walk_bfs_sub(walk_sub_width, walk_sub_height, ssx, ssy, stx, sty, &new_len))
            continue;
        walk_target_x = nx;
        walk_target_y = ny;
        walk_target_sub_x = stx;
        walk_target_sub_y = sty;
        walk_path_len = new_len;
        walk_path_idx = 0;
        walk_item_round = 0;                /* fresh rounding budget per side */
        walk_stall_ticks = 0;
        walk_stall_px = guybrush[current_nation].px;
        walk_stall_p2y = guybrush[current_nation].p2y;
        walk_phase = WALK_PHASE_PATH;
        return true;
    }
    return false;
}

/* Corner-rounding exhausted (all WALK_ITEM_ROUND_MAX rounds tried with no
 * progress): first cycle to an untried approach side of the prop tile
 * (walk_item_try_next_side above); only when every side has been tried,
 * fall back to the generic sidestep-then-re-BFS recovery plain-tile
 * PATH-phase stalls use (WALK_PHASE_SIDESTEP, gated by the shared
 * walk_recovery_count budget); a stall once that budget is exhausted is
 * blocked immediately. */
static void walk_item_round_exhausted(int16_t px, int16_t p2y, bool primary_is_x)
{
    walk_item_rounding = false;
    if (walk_item_try_next_side()) {
        walk_release_keys();
        return;
    }
    if (walk_recovery_count < WALK_MAX_RECOVERIES) {
        /* walk_target_x/y is still the item's (unreached) tile target at
         * this point -- walk_item_try_next_side above already failed to
         * find a next side, so it never reassigned them. Derive the
         * primary retry direction (see walk_sidestep_primary_dir's header
         * comment on walk_pump_sidestep) the same way PATH-phase's own
         * SIDESTEP entry does, from that target tile's center. */
        int16_t wcx = (int16_t)(walk_target_x * 32 + 16);
        int16_t wcy = (int16_t)(walk_target_y * 32 + 16);
        walk_recovery_count++;
        walk_phase = WALK_PHASE_SIDESTEP;
        if (primary_is_x) {
            walk_sidestep_dir[0] = WALK_DIR_DOWN;
            walk_sidestep_dir[1] = WALK_DIR_UP;
            walk_sidestep_primary_dir = (px < wcx) ? WALK_DIR_RIGHT : WALK_DIR_LEFT;
        } else {
            walk_sidestep_dir[0] = WALK_DIR_RIGHT;
            walk_sidestep_dir[1] = WALK_DIR_LEFT;
            walk_sidestep_primary_dir = (p2y < wcy) ? WALK_DIR_DOWN : WALK_DIR_UP;
        }
        walk_sidestep_idx = 0;
        walk_sidestep_ticks = 0;
        walk_hold_only(walk_dir_key(walk_sidestep_dir[0]));
        walk_stall_ticks = 0;
        walk_stall_px = px;
        walk_stall_p2y = p2y;
        return;
    }
    walk_set_blocked(walk_classify_stall());
}

/* Runs one tick of the current corner-rounding round (walk_pump_item_pixel
 * dispatches here whenever walk_item_rounding is set). Uses the primary
 * axis captured at the round's start (walk_item_round_primary_is_x), not
 * a fresh per-tick recompute, so a momentary in-range dip on that axis
 * mid-round doesn't reinterpret which direction is being sidestepped
 * (brief: "temporary leaving of an in-range axis during rounding is
 * allowed"). May recurse once, directly into the next round's first tick,
 * when a re-attempt fails and another round is still available -- avoids
 * wasting a whole tick on a no-op transition. */
static void walk_pump_item_round(int16_t px, int16_t p2y,
                                  int16_t dx, int16_t dy,
                                  int16_t adx, int16_t ady)
{
    bool primary_is_x = walk_item_round_primary_is_x;
    int16_t primary_dist = primary_is_x ? adx : ady;
    uint8_t primary_key = primary_is_x
        ? (dx < 0 ? walk_key_right : walk_key_left)
        : (dy < 0 ? walk_key_down  : walk_key_up);

    walk_item_round_ticks++;

    if (walk_item_round_sidestepping) {
        int16_t disp_x = (int16_t)(px - walk_item_round_ref_px);
        int16_t disp_y = (int16_t)((p2y - walk_item_round_ref_p2y) / 2);
        int16_t perp_disp = primary_is_x ? disp_y : disp_x;
        int16_t aperp = (int16_t)(perp_disp < 0 ? -perp_disp : perp_disp);
        uint8_t perp_key_a = primary_is_x ? walk_key_down : walk_key_right;
        uint8_t perp_key_b = primary_is_x ? walk_key_up   : walk_key_left;
        uint8_t perp_key = (walk_item_round_side == 0) ? perp_key_a : perp_key_b;

        if (aperp >= walk_item_round_target_px[walk_item_round - 1] ||
            walk_item_round_ticks >= WALK_ITEM_ROUND_SIDESTEP_CAP_TICKS) {
            /* Sidestep done (or gave up because it's itself blocked) --
             * move on to the re-attempt sub-step. */
            walk_item_round_sidestepping = false;
            walk_item_round_ticks = 0;
            walk_item_round_best_dist = primary_dist;
            walk_hold_only(primary_key);
            return;
        }
        walk_hold_only(perp_key);
        return;
    }

    /* Re-attempt sub-step: did pushing the primary direction again get any
     * closer than it was right when the sidestep ended? */
    if (primary_dist < walk_item_round_best_dist) {
        walk_item_rounding = false;
        walk_item_progress_axis_is_x = primary_is_x;
        walk_item_progress_best = primary_dist;
        walk_item_progress_ticks = 1;
        walk_hold_only(primary_key);
        return;
    }
    if (walk_item_round_ticks >= WALK_ITEM_ROUND_REATTEMPT_TICKS) {
        if (walk_item_round < WALK_ITEM_ROUND_MAX) {
            walk_item_start_round(px, p2y, primary_is_x);
            walk_pump_item_round(px, p2y, dx, dy, adx, ady);
            return;
        }
        walk_item_round_exhausted(px, p2y, primary_is_x);
        return;
    }
    walk_hold_only(primary_key);
}

static void walk_pump_item_pixel(void)
{
    int16_t px, p2y, p2y_half, dx, dy, adx, ady;
    bool x_ok, y_ok, primary_is_x;
    int16_t primary_dist;

    px = guybrush[current_nation].px;
    p2y = guybrush[current_nation].p2y;
    p2y_half = (int16_t)(p2y / 2);
    dx = (int16_t)(px - walk_item_anchor_x);
    dy = (int16_t)(p2y_half - walk_item_anchor_y);
    adx = (int16_t)(dx < 0 ? -dx : dx);
    ady = (int16_t)(dy < 0 ? -dy : dy);

    x_ok = adx <= WALK_ITEM_MARGIN;
    y_ok = ady <= WALK_ITEM_MARGIN;
    if (x_ok && y_ok) {
        walk_cancel(WALK_ARRIVED);
        if (walk_item_pickup)
            enqueue_key(KEY_INVENTORY_PICKUP, 100);
        return;
    }

    if (walk_item_rounding) {
        walk_pump_item_round(px, p2y, dx, dy, adx, ady);
        return;
    }

    primary_is_x = adx > WALK_ITEM_DEADBAND;   /* x settled -> hand primary to y */
    primary_dist = primary_is_x ? adx : ady;

    if (walk_item_progress_axis_is_x != primary_is_x ||
        primary_dist < walk_item_progress_best) {
        walk_item_progress_axis_is_x = primary_is_x;
        walk_item_progress_best = primary_dist;
        walk_item_progress_ticks = 1;
    } else {
        walk_item_progress_ticks++;
    }

    if (walk_item_progress_ticks >= WALK_ITEM_PROGRESS_TICKS) {
        if (walk_item_round < WALK_ITEM_ROUND_MAX) {
            walk_item_start_round(px, p2y, primary_is_x);
            walk_pump_item_round(px, p2y, dx, dy, adx, ady);
            return;
        }
        walk_item_round_exhausted(px, p2y, primary_is_x);
        return;
    }

    walk_hold_only(primary_is_x
        ? (dx < 0 ? walk_key_right : walk_key_left)
        : (dy < 0 ? walk_key_down  : walk_key_up));
}

/* Called every tick from agent_api_tick(), right after input_pump() (see
 * its call site) so a walk drives direction keys on the same cadence a
 * real held key would. WALK_PHASE_PATH holds key_down[KEY_DIRECTION_*]
 * toward the current waypoint's tile center; two direction keys held
 * together give the engine's own diagonal motion (main.c:915-923) for
 * free. WALK_PHASE_CROSS and WALK_PHASE_SIDESTEP (Task 9) hold a single
 * direction key at a time -- see their own header comments above. */
static void walk_pump(void)
{
    int16_t px, p2y, tile_x, tile_y, target_x, target_y, cx, cy;

    if (walk_status != WALK_WALKING) return;

    /* Prisoner switch (agent-initiated via /input, or a human at the real
     * keyboard) invalidates the walk's whole premise -- stop and call it
     * blocked, per the brief. Not a stall (nothing to classify), so this
     * uses the generic "static" bucket rather than walk_classify_stall(). */
    if (current_nation != walk_nation) { walk_set_blocked(WALK_BLOCK_STATIC); return; }

    /* Whole-walk hard cap (part B): bounds path-following + one
     * sidestep-recovery attempt + crossing all taking their maximum time
     * in sequence, protecting against any pathological loop. Always
     * "timeout" regardless of which phase it interrupted -- the 30s cap is
     * its own distinct, unambiguous cause. */
    if (++walk_total_ticks > WALK_MAX_TICKS) { walk_set_blocked(WALK_BLOCK_TIMEOUT); return; }

    /* Room change is success on every phase -- the walk got the prisoner
     * out of the room. This doubles as the CROSSING phase's actual
     * success signal (see its header comment): reaching it there needs no
     * phase-specific check of its own. */
    if (guybrush[current_nation].room != walk_room) { walk_cancel(WALK_ARRIVED); return; }

    if (walk_phase == WALK_PHASE_CROSS)      { walk_pump_cross();      return; }
    if (walk_phase == WALK_PHASE_SIDESTEP)   { walk_pump_sidestep();   return; }
    if (walk_phase == WALK_PHASE_ITEM_PIXEL) { walk_pump_item_pixel(); return; }

    /* WALK_PHASE_PATH: follow the BFS path -- TILE-resolution waypoints
     * (Task 10: now derived from a mask-accurate SUBCELL reachability
     * search via walk_bfs_sub/walk_path_to_tiles, see their header
     * comments for why the search runs at subcell resolution but the
     * waypoints it hands to this steering loop are still tiles) -- plus
     * the stall -> one-shot sidestep-recovery hook (part B) and the
     * target-reached -> CROSSING-phase handoff (part A). */
    px  = guybrush[current_nation].px;
    p2y = guybrush[current_nation].p2y;
    tile_x = px / 32;
    tile_y = p2y / 32;

    if (px == walk_stall_px && p2y == walk_stall_p2y) {
        if (++walk_stall_ticks >= WALK_STALL_LIMIT) {
            /* An exit-walk stall while already ADJACENT (one tile away,
             * either axis) to the doorway tile hands off directly to
             * WALK_PHASE_CROSS instead of the generic sidestep/blocked
             * path below. A doorway's real passable column is a property
             * of the CROSSING itself, not of the approach tile's own
             * floor mask -- verified live: room 253's [1,7] door has its
             * genuine opening well off the tile's geometric center (and
             * off the mask-validated-open-floor point too), so PATH-phase
             * approaching it can stall one tile short no matter which
             * point within the tile it targets, while WALK_PHASE_CROSS's
             * perpendicular deadband correction (already proven, see its
             * own header comment) finds and holds the real alignment once
             * given the chance to try. Only for exit walks (walk_target_x/y
             * is a real doorway then) and only when adjacency is genuine
             * (Manhattan tile distance 1) -- otherwise falls through to
             * the same sidestep/blocked handling every other stall uses. */
            if (walk_is_exit_target &&
                (abs((int)tile_x - (int)walk_target_x) +
                 abs((int)tile_y - (int)walk_target_y)) <= 1) {
                walk_phase = WALK_PHASE_CROSS;
                walk_cross_ncand = walk_exit_dir_candidates(walk_target_x, walk_target_y,
                                                             walk_cross_cand);
                walk_cross_idx = 0;
                walk_cross_round = 0;
                walk_cross_ticks = 0;
                walk_pump_cross();
                return;
            }
            /* A final-waypoint stall on an item walk does NOT get the
             * generic perpendicular-sidestep-then-re-BFS recovery below --
             * that recovery re-paths back to this SAME coarse target tile,
             * which is exactly what's unreachable (verified live: room
             * 251's lockpick sits against a bunk bed whose collision mask
             * fully blocks the straight tile-to-tile approach, on every
             * column the generic sidestep can reach in its bounded time).
             * Instead, hand off directly to WALK_PHASE_ITEM_PIXEL from
             * wherever we're stalled: its pixel-window arrival test can
             * already be satisfied without ever completing the coarse
             * tile crossing (e.g. reachable by angling around the bed's
             * corner into an adjacent tile column while still landing
             * inside the anchor's 17px-wide window), and its own
             * corner-rounding stall recovery (see walk_pump_item_round's
             * header comment) is built for exactly this furniture-corner
             * case, unlike the single-axis sidestep here. Uses the walk's
             * one shared recovery attempt only if ITEM_PIXEL's own
             * corner-rounding budget ends up exhausted, not this
             * handoff. */
            if (walk_is_item_target && walk_path_idx >= walk_path_len - 1) {
                walk_phase = WALK_PHASE_ITEM_PIXEL;
                walk_stall_ticks = 0;
                walk_stall_px = px;
                walk_stall_p2y = p2y;
                walk_pump_item_pixel();
                return;
            }
            if (walk_recovery_count < WALK_MAX_RECOVERIES && walk_path_idx < walk_path_len - 1) {
                /* Non-final-waypoint stall, recovery budget not exhausted:
                 * sidestep perpendicular to the current leg's direction
                 * of travel. Each BFS step (in the tile path
                 * walk_path_to_tiles derives from the subcell search) is
                 * single-axis, so comparing the waypoint tile we were
                 * heading for against our current tile unambiguously
                 * tells us which axis was "primary" (blocked) and which
                 * is perpendicular (the way around). */
                int16_t wtx = walk_path_x[walk_path_idx];
                int16_t wty = walk_path_y[walk_path_idx];
                int16_t wcx = (int16_t)(wtx * 32 + 16);
                int16_t wcy = (int16_t)(wty * 32 + 16);
                walk_recovery_count++;
                walk_phase = WALK_PHASE_SIDESTEP;
                if (wtx != tile_x) {
                    walk_sidestep_dir[0] = WALK_DIR_DOWN;
                    walk_sidestep_dir[1] = WALK_DIR_UP;
                    walk_sidestep_primary_dir = (px < wcx) ? WALK_DIR_RIGHT : WALK_DIR_LEFT;
                } else {
                    walk_sidestep_dir[0] = WALK_DIR_RIGHT;
                    walk_sidestep_dir[1] = WALK_DIR_LEFT;
                    walk_sidestep_primary_dir = (p2y < wcy) ? WALK_DIR_DOWN : WALK_DIR_UP;
                }
                walk_sidestep_idx = 0;
                walk_sidestep_ticks = 0;
                walk_hold_only(walk_dir_key(walk_sidestep_dir[0]));
                walk_stall_ticks = 0;
                return;
            }
            walk_set_blocked(walk_classify_stall());
            return;
        }
    } else {
        walk_stall_ticks = 0;
        walk_stall_px = px;
        walk_stall_p2y = p2y;
    }

    /* Advance past any waypoints already reached (normally just one tile
     * per tick, but a single tick could in principle cross more than one
     * waypoint -- this loop handles that without special-casing it). */
    while (walk_path_idx < walk_path_len &&
           tile_x == walk_path_x[walk_path_idx] &&
           tile_y == walk_path_y[walk_path_idx])
        walk_path_idx++;

    if (walk_path_idx >= walk_path_len) {
        if (walk_is_exit_target) {
            /* Target reached on an exit walk: don't call it arrived yet
             * -- hand off to the CROSSING phase to actually push through
             * the doorway (part A). */
            walk_phase = WALK_PHASE_CROSS;
            walk_cross_ncand = walk_exit_dir_candidates(walk_target_x, walk_target_y,
                                                         walk_cross_cand);
            walk_cross_idx = 0;
            walk_cross_round = 0;
            walk_cross_ticks = 0;
            /* Set this tick's keys immediately (rather than waiting for
             * the next tick) so no tick is spent still holding whatever
             * PATH-phase was steering with a moment ago. */
            walk_pump_cross();
            return;
        }
        if (walk_is_item_target) {
            /* Target tile reached on an item walk: hand off to the
             * pixel-precision ITEM_PIXEL phase rather than declaring
             * arrived on tile precision alone (see its header comment).
             * Fresh stall window for the new phase, same pattern the
             * SIDESTEP->PATH resume above uses. */
            walk_phase = WALK_PHASE_ITEM_PIXEL;
            walk_stall_ticks = 0;
            walk_stall_px = guybrush[current_nation].px;
            walk_stall_p2y = guybrush[current_nation].p2y;
            walk_pump_item_pixel();
            return;
        }
        walk_cancel(WALK_ARRIVED);
        return;
    }

    target_x = walk_path_x[walk_path_idx];
    target_y = walk_path_y[walk_path_idx];
    if (walk_path_idx == walk_path_len - 1 && !walk_is_exit_target) {
        /* Final waypoint of a NON-exit walk: aim at the mask-validated
         * WALKABLE subcell within this tile (walk_target_sub_x/y), not
         * the tile's naive geometric center -- see walk_target_sub_x/y's
         * own header comment for the concrete failure this fixes (a
         * partially-blocked tile whose real open area isn't centered, so
         * the plain-center aim point was itself sitting on a wall pixel
         * and unreachable no matter how PATH-phase steered).
         *
         * Exit walks are deliberately excluded: an exit tile's mask
         * defines its OWN floor, not the doorway's actual through-
         * passage into the next room, which can require a different
         * (narrower, specifically-aligned) column than any single point
         * this tile's own mask validates as "open" -- verified live: a
         * subcell that passed this tile's mask test sat right next to a
         * real wall, while the doorway's genuine passable column was
         * several pixels further over. WALK_PHASE_CROSS already exists
         * specifically to find and hold that real crossing alignment
         * (perpendicular deadband correction while pushing outward, see
         * its own header comment) -- PATH-phase only needs to get
         * "close enough" to hand off to it, exactly like the pre-Task10
         * design already proved reliable. */
        cx = (int16_t)(walk_target_sub_x * SUB_SIZE + SUB_SIZE/2);
        cy = (int16_t)(walk_target_sub_y * SUB_SIZE + SUB_SIZE/2);
    } else {
        cx = (int16_t)(target_x * 32 + 16);
        cy = (int16_t)(target_y * 32 + 16);
    }

    {
        bool x_out = (px < cx - WALK_DEADBAND) || (px > cx + WALK_DEADBAND);
        bool y_out = (p2y < cy - WALK_DEADBAND) || (p2y > cy + WALK_DEADBAND);
        /* Final waypoint only: correct ONE axis at a time (x first, same
         * priority ITEM_PIXEL uses), not both simultaneously. The engine
         * rejects a diagonal move atomically if EITHER component axis
         * would collide (brief, verbatim) -- fine for intermediate
         * waypoints (mid-tile, both axes usually have a full tile's worth
         * of open floor either side) but verified live to make a tight
         * doorway/subcell-precision final approach fail outright: this
         * exact tile's real (mask-validated) opening needed x and y
         * corrected in sequence, not at once, even though EACH single
         * axis, held alone, moves the prisoner fine (confirmed via manual
         * /input single-key holds from the exact stuck position). Every
         * other waypoint keeps the original simultaneous-diagonal
         * steering (main.c's own diagonal motion, "for free" per the
         * pre-Task10 comment) since it's never needed anything stricter. */
        bool final_wp = (walk_path_idx == walk_path_len - 1) && !walk_is_exit_target;
        bool drive_x = x_out;
        bool drive_y = y_out && (!final_wp || !x_out);

        if (drive_x && px < cx - WALK_DEADBAND) {
            key_down[walk_key_right] = true;
            key_down[walk_key_left] = false;  key_readonce[walk_key_left] = false;
        } else if (drive_x && px > cx + WALK_DEADBAND) {
            key_down[walk_key_left] = true;
            key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
        } else {
            key_down[walk_key_left] = false;  key_readonce[walk_key_left] = false;
            key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
        }

        if (drive_y && p2y < cy - WALK_DEADBAND) {
            key_down[walk_key_down] = true;
            key_down[walk_key_up] = false;    key_readonce[walk_key_up] = false;
        } else if (drive_y && p2y > cy + WALK_DEADBAND) {
            key_down[walk_key_up] = true;
            key_down[walk_key_down] = false;  key_readonce[walk_key_down] = false;
        } else {
            key_down[walk_key_up] = false;    key_readonce[walk_key_up] = false;
            key_down[walk_key_down] = false;  key_readonce[walk_key_down] = false;
        }
    }
}

/* Looks up `name` in prop_name[1..NB_PROPS-1] (skipping ITEM_NONE), by
 * exact match. Returns the item id, or -1 if `name` isn't a known prop
 * name. Shared by the item-mode branch of handle_walk below. */
static int item_id_for_name(const char* name)
{
    int j;
    for (j = 1; j < NB_PROPS; j++)
        if (!strcmp(prop_name[j], name))
            return j;
    return -1;
}

/* Finds item `item_id` among the CURRENT room's visible props (identical
 * scan, skip conditions, and pixel-anchor formula as handle_room's `items`
 * block -- see its comment for why: fair play means an item walk may only
 * ever target a prop a human could actually see rendered on screen, same
 * as /room already exposes and nothing more). On a match, fills
 * *out_anchor_x/y (the exact pickup-trigger pixel anchor) and
 * *out_tile_x/y (tile coordinates, NOT yet clamped to the room's own
 * width/height -- the caller clamps, matching /room) and returns true. */
static bool find_room_item(uint8_t item_id, bool outside,
                            int16_t* out_anchor_x, int16_t* out_anchor_y,
                            int16_t* out_tile_x, int16_t* out_tile_y)
{
    uint16_t u;
    for (u = 0; u < nb_room_props; u++) {
        uint16_t prop_offset = room_props[u];
        uint8_t this_id;
        uint16_t raw_x, raw_y;
        int16_t itile_x, itile_y;

        if (prop_offset == 0)
            continue;
        this_id = readbyte(fbuffer[OBJECTS], prop_offset + 7);
        if (this_id != item_id)
            continue;

        raw_x = readword(fbuffer[OBJECTS], prop_offset + 4);
        raw_y = readword(fbuffer[OBJECTS], prop_offset + 2);
        itile_x = (int16_t)((raw_x - 15) / 32);
        itile_y = (int16_t)((raw_y - 4) / 16);

        if (outside) {
            if (itile_x < 0 || itile_y < 0 ||
                itile_x >= CMP_MAP_WIDTH || itile_y >= CMP_MAP_HEIGHT)
                continue;
            if (remove_props[itile_x][itile_y])
                continue;
        }

        *out_anchor_x = (int16_t)(raw_x - 15);
        *out_anchor_y = (int16_t)(raw_y - 4);
        *out_tile_x = itile_x;
        *out_tile_y = itile_y;
        return true;
    }
    return false;
}

/* POST /walk {"tile":[x,y]} | {"exit":N} | {"item":"<name>","pickup":bool}
 * | {"cancel":true}. See docs/AGENT-API.md for the full contract. Any
 * /input request that arrives while a walk is in progress cancels the walk
 * first (handle_input, per the brief); any /walk request that arrives
 * while the input queue is busy is rejected with 409 rather than
 * interleaving the two key-holding mechanisms. */
static void handle_walk(int cfd, const char* body)
{
    long a = 0, b = 0, exit_idx;
    uint16_t width = 0, height = 0;
    int16_t ssx, ssy, stx, sty, tx = 0, ty = 0;
    int snap, path_len = 0;
    bool is_exit_target;
    char item_name[24];
    bool has_item = false, item_pickup = false;
    int16_t item_anchor_x = 0, item_anchor_y = 0;
    char resp[96]; int n;

    if (body) {
        const char* p = strstr(body, "\"cancel\"");
        if (p && strstr(p, "true")) {
            walk_cancel(WALK_IDLE);
            walk_blocked_reason = WALK_BLOCK_NONE;
            send_response(cfd, 200, "application/json", "{\"walking\":false}", 18);
            return;
        }
    }

    if (agent_input_queue_depth() > 0) {
        static const char* busy = "{\"error\":\"input busy\"}";
        send_response(cfd, 409, "application/json", busy, strlen(busy));
        return;
    }

    has_item = body && json_str(body, "item", item_name, sizeof(item_name));
    exit_idx = (!has_item && body) ? json_int(body, "exit", -1) : -1;

    if (has_item) {
        uint16_t room = guybrush[current_nation].room;
        bool outside = (room == ROOM_OUTSIDE);
        int item_id;
        int16_t itile_x = 0, itile_y = 0;

        {
            const char* p = strstr(body, "\"pickup\"");
            item_pickup = p && strstr(p, "true");
        }

        snap = walk_snapshot_grid(&width, &height, -1, NULL, NULL);
        if (snap == WALK_SNAP_NO_ROOM) {
            static const char* unavail = "room data unavailable";
            send_response(cfd, 400, "text/plain", unavail, strlen(unavail));
            return;
        }

        item_id = item_id_for_name(item_name);
        if (item_id < 0 ||
            !find_room_item((uint8_t)item_id, outside,
                             &item_anchor_x, &item_anchor_y, &itile_x, &itile_y)) {
            static const char* noitem = "no such item here";
            send_response(cfd, 400, "text/plain", noitem, strlen(noitem));
            return;
        }

        /* Clamp to the room grid the same way /room's items block does,
         * before indexing walk_grid_walkable[] with it. */
        if (itile_x < 0) itile_x = 0;
        if (itile_y < 0) itile_y = 0;
        if (itile_x >= (int16_t)width)  itile_x = (int16_t)width  - 1;
        if (itile_y >= (int16_t)height) itile_y = (int16_t)height - 1;

        tx = itile_x; ty = itile_y;
        walk_item_prop_tx = itile_x;
        walk_item_prop_ty = itile_y;
        walk_item_tried_mask = 0;

        /* Optional "from":"e|w|s|n" approach-side hint (campaign knowledge
         * from an earlier grab): start at that neighbor of the prop tile
         * directly instead of discovering the good side by cycling. */
        {
            char from_hint[4] = "";
            if (json_str(body, "from", from_hint, sizeof(from_hint))) {
                static const int8_t hdx[4] = { 1, -1, 0, 0 };   /* e w s n */
                static const int8_t hdy[4] = { 0, 0, 1, -1 };
                const char* order = "ewsn";
                const char* pos = strchr(order, (from_hint[0] | 0x20));
                if (pos) {
                    int hi = (int)(pos - order);
                    int16_t hx = (int16_t)(itile_x + hdx[hi]);
                    int16_t hy = (int16_t)(itile_y + hdy[hi]);
                    if (hx >= 0 && hy >= 0 && hx < (int16_t)width &&
                        hy < (int16_t)height &&
                        walk_grid_walkable[(int)hy*(int)width + hx]) {
                        tx = hx; ty = hy;
                        walk_item_tried_mask = (uint8_t)(1 << hi);
                    }
                }
            }
        }

        if (!walk_grid_walkable[(int)ty*(int)width + tx] && tx == itile_x && ty == itile_y) {
            /* Prop's own tile is unwalkable (furniture/void underneath it)
             * -- per the brief, fall back to the nearest walkable tile
             * adjacent to it (4-neighborhood); no such neighbor -> no
             * path, same 409 shape walk_bfs failure uses below. Mark the
             * chosen side as tried so approach-side cycling starts from
             * the next one. */
            static const int8_t adx[4] = { 1, -1, 0, 0 };
            static const int8_t ady[4] = { 0, 0, 1, -1 };
            bool found_adj = false;
            int i;
            for (i = 0; i < 4; i++) {
                int16_t nx = (int16_t)(itile_x + adx[i]);
                int16_t ny = (int16_t)(itile_y + ady[i]);
                if (nx < 0 || ny < 0 || nx >= (int16_t)width || ny >= (int16_t)height)
                    continue;
                if (walk_grid_walkable[(int)ny*(int)width + nx]) {
                    tx = nx; ty = ny;
                    walk_item_tried_mask = (uint8_t)(1 << i);
                    found_adj = true;
                    break;
                }
            }
            if (!found_adj) {
                static const char* nopath = "{\"error\":\"no path\"}";
                send_response(cfd, 409, "application/json", nopath, strlen(nopath));
                return;
            }
        }
        /* An item walk is never a doorway crossing, regardless of whether
         * the resolved tile happens to also be an exit cell. */
        is_exit_target = false;
    } else if (exit_idx >= 0) {
        snap = walk_snapshot_grid(&width, &height, (int)exit_idx, &tx, &ty);
        is_exit_target = true;
    } else if (body && json_intpair(body, "tile", &a, &b)) {
        snap = walk_snapshot_grid(&width, &height, -1, NULL, NULL);
        tx = (int16_t)a; ty = (int16_t)b;
        is_exit_target = false;    /* refined below once walk_grid_isexit is known */
    } else {
        static const char* need =
            "expected \"tile\":[x,y], \"exit\":N, or \"item\":\"name\"";
        send_response(cfd, 400, "text/plain", need, strlen(need));
        return;
    }

    if (!has_item) {
        if (snap == WALK_SNAP_NO_ROOM) {
            static const char* unavail = "room data unavailable";
            send_response(cfd, 400, "text/plain", unavail, strlen(unavail));
            return;
        }
        if (snap == WALK_SNAP_BAD_EXIT) {
            static const char* badexit = "invalid exit index";
            send_response(cfd, 400, "text/plain", badexit, strlen(badexit));
            return;
        }

        if (tx < 0 || ty < 0 || tx >= (int16_t)width || ty >= (int16_t)height) {
            static const char* oob = "tile out of bounds";
            send_response(cfd, 400, "text/plain", oob, strlen(oob));
            return;
        }
        /* walk_grid_walkable already folds in exit cells (readexit & 0x1F)
         * even when the underlying tile id is 0 -- see walk_snapshot_grid
         * -- so an {"exit":N} target that lands on a void tile is still
         * accepted here, matching /room's 'E' overlay on void cells. */
        if (!walk_grid_walkable[(int)ty*(int)width + tx]) {
            static const char* voidtile = "target is void tile";
            send_response(cfd, 400, "text/plain", voidtile, strlen(voidtile));
            return;
        }

        /* Part A: an explicit {"exit":N} request is always an exit walk; a
         * {"tile":[x,y]} request is one too if it happens to land on an
         * exit cell (walk_grid_isexit[], populated by the same scan that
         * just built walk_grid_walkable[] above) -- gates the
         * target-reached -> CROSSING-phase handoff in walk_pump. */
        is_exit_target = is_exit_target || walk_grid_isexit[(int)ty*(int)width + tx];
    }

    /* Task 10: resolve the tile-level target (tx,ty) -- an exit tile, a
     * plain {"tile":[x,y]} request, or an item's own/approach tile -- down
     * to a concrete WALKABLE subcell (mask-accurate, not just "this tile
     * has a nonzero id"), biased toward the TILE'S OWN CENTER in every
     * case (including item walks -- NOT the item's exact pickup anchor;
     * see walk_sidestep_finish's matching comment for why: PATH-phase only
     * needs to reach the general vicinity of the target tile, same as a
     * plain-tile walk, and hands off to WALK_PHASE_ITEM_PIXEL's dedicated
     * pixel-precision/corner-rounding machinery for the final approach).
     * This is what actually fixes the "BFS is tile-blind" weakness: a
     * partially-furniture-blocked tile no longer routes the prisoner
     * straight at the blocked part only to discover it via a stall-
     * recovery cycle -- the mask-accurate grid steers the BFS toward
     * whichever part of the tile is genuinely open. */
    walk_sub_clamp_start(&ssx, &ssy);
    walk_resolve_target_subcell(tx, ty, (int16_t)(tx*32 + 16),
                                 (int16_t)(ty*32 + 16), &stx, &sty);

    if (!walk_bfs_sub(walk_sub_width, walk_sub_height, ssx, ssy, stx, sty, &path_len)) {
        static const char* nopath = "{\"error\":\"no path\"}";
        send_response(cfd, 409, "application/json", nopath, strlen(nopath));
        return;
    }

    /* A fresh /walk supersedes any prior one -- walk_cancel() releases
     * keys first if one was still in progress, matching every other
     * termination path before this new state takes over. */
    walk_cancel(WALK_IDLE);

    walk_path_len = path_len;
    walk_path_idx = 0;
    walk_nation = current_nation;
    walk_room = guybrush[current_nation].room;
    walk_stall_px = guybrush[current_nation].px;
    walk_stall_p2y = guybrush[current_nation].p2y;
    walk_stall_ticks = 0;
    walk_key_up    = KEY_DIRECTION_UP;
    walk_key_down  = KEY_DIRECTION_DOWN;
    walk_key_left  = KEY_DIRECTION_LEFT;
    walk_key_right = KEY_DIRECTION_RIGHT;
    /* Task 9 state: fresh snapshot dimensions/target/start for the
     * CROSSING-direction heuristic and any sidestep-recovery re-path,
     * phase reset to PATH, the one-shot recovery latch cleared, and the
     * whole-walk tick budget restarted. */
    walk_width = width;
    walk_height = height;
    walk_target_x = tx;
    walk_target_y = ty;
    walk_target_sub_x = stx;
    walk_target_sub_y = sty;
    /* walk_start_x/y is walk_exit_dir_candidates' fallback (walk_path_len
     * < 2) arrival-direction reference, which compares against TILE
     * waypoints -- must be the start TILE, not the subcell BFS's start
     * subcell (ssx/ssy) passed to walk_bfs_sub above. */
    walk_start_x = (int16_t)(ssx / SUB_PER_TILE);
    walk_start_y = (int16_t)(ssy / SUB_PER_TILE);
    walk_is_exit_target = is_exit_target;
    /* Item-walk state (the /walk item+pickup feature): gates PATH-phase's
     * target-reached handoff into WALK_PHASE_ITEM_PIXEL. Explicitly reset
     * to false/0 for every non-item walk too, since these are persistent
     * globals reused across /walk calls. */
    walk_is_item_target = has_item;
    walk_item_pickup = has_item && item_pickup;
    walk_item_anchor_x = item_anchor_x;
    walk_item_anchor_y = item_anchor_y;
    walk_item_progress_axis_is_x = false;
    walk_item_progress_best = 0;
    walk_item_progress_ticks = 0;
    walk_item_round = 0;
    walk_item_rounding = false;
    walk_item_round_sidestepping = false;
    walk_phase = WALK_PHASE_PATH;
    walk_recovery_count = 0;
    walk_total_ticks = 0;
    walk_blocked_reason = WALK_BLOCK_NONE;
    walk_status = WALK_WALKING;

    if (has_item)
        n = snprintf(resp, sizeof(resp),
                     "{\"walking\":true,\"target\":[%d,%d],\"path_len\":%d,"
                     "\"item\":\"%s\"}",
                     (int)tx, (int)ty, path_len, item_name);
    else
        n = snprintf(resp, sizeof(resp),
                     "{\"walking\":true,\"target\":[%d,%d],\"path_len\":%d}",
                     (int)tx, (int)ty, path_len);
    send_response(cfd, 202, "application/json", resp, (size_t)n);
}

static void handle_request(int cfd)
{
    static char req_buf[4096];
    char method[8] = "", path[64] = "";
    ssize_t n = recv(cfd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) return;
    req_buf[n] = '\0';
    if (sscanf(req_buf, "%7s %63s", method, path) != 2) {
        send_response(cfd, 400, "text/plain", "bad request", 11);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/state"))
        handle_state(cfd);
    else if (!strcmp(method, "GET") && !strcmp(path, "/screen"))
        handle_screen(cfd);
    else if (!strcmp(method, "GET") && !strcmp(path, "/room"))
        handle_room(cfd);
    else if (!strcmp(method, "POST") && !strcmp(path, "/input")) {
        char* hdr_end = strstr(req_buf, "\r\n\r\n");
        char* body = hdr_end ? hdr_end + 4 : NULL;
        handle_input(cfd, body);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/control")) {
        char* hdr_end = strstr(req_buf, "\r\n\r\n");
        char* body = hdr_end ? hdr_end + 4 : NULL;
        handle_control(cfd, body);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/say")) {
        char* hdr_end = strstr(req_buf, "\r\n\r\n");
        char* body = hdr_end ? hdr_end + 4 : NULL;
        handle_say(cfd, body);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/walk")) {
        char* hdr_end = strstr(req_buf, "\r\n\r\n");
        char* body = hdr_end ? hdr_end + 4 : NULL;
        handle_walk(cfd, body);
    } else
        send_response(cfd, 404, "text/plain", "not found", 9);
}

/* Draining only one pending connection per tick (the original behavior)
 * means a burst of back-to-back requests -- e.g. a test script or agent
 * issuing several rapid sequential curls -- can outrun the accept() rate
 * and pile up in the kernel's listen backlog; a connection that arrives
 * right as the backlog is full is refused outright (client sees an empty
 * reply, not an HTTP error, since nothing ever got far enough to call
 * send_response). Draining a bounded batch per tick keeps each individual
 * request's cost the same (still capped by SO_RCVTIMEO/SNDTIMEO and
 * SEND_RESPONSE_DEADLINE_MS below) while making it very unlikely the
 * backlog (now 16, see agent_api_init) ever actually fills under normal
 * sequential-client usage. */
#define AGENT_API_MAX_ACCEPTS_PER_TICK 8

void agent_api_tick(void)
{
    struct timeval tv = { 0, 200000 };  // 200 ms cap per request
    int cfd, accepted;
    input_pump();   /* first: held keys release on schedule even with no
                     * client connected (and even if listen_fd is closed). */
    walk_pump();    /* right after input_pump: drives walk-held direction
                     * keys on the same per-tick cadence, independent of
                     * whether a client is connected this tick. */
    if (listen_fd < 0) return;
    for (accepted = 0; accepted < AGENT_API_MAX_ACCEPTS_PER_TICK; accepted++) {
        cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) return;
        /* Root cause of the intermittent-empty-response flakiness under
         * rapid repeated requests (proved with an isolated repro, not
         * guessed): on this platform (macOS/BSD sockets), a socket
         * returned by accept() INHERITS the O_NONBLOCK flag of the
         * listening socket (listen_fd is deliberately non-blocking so the
         * accept loop above never stalls waiting for a new connection).
         * SO_RCVTIMEO/SO_SNDTIMEO only bound how long a *blocking* call
         * waits -- they are silently no-ops on a non-blocking socket, where
         * recv() instead returns -1/EAGAIN immediately if the client's
         * request bytes haven't arrived yet (a real race: accept() can
         * complete as soon as the TCP handshake finishes, which can win
         * against the client's own write() under load/rapid-fire connects).
         * handle_request()'s `if (n <= 0) return;` then silently closes the
         * connection with zero bytes written -- the client sees an empty
         * reply. Clearing O_NONBLOCK here makes cfd a genuinely blocking
         * socket, so SO_RCVTIMEO/SO_SNDTIMEO (set right after) do what
         * their names say: recv() blocks (up to 200ms) until the request
         * actually arrives instead of failing on an accept/write race. */
        fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) & ~O_NONBLOCK);
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_request(cfd);
        close(cfd);
    }
}
#endif
