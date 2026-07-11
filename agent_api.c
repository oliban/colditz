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
        listen(listen_fd, 4) < 0)
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
    n = json_append(json, sizeof(json), n, "],\"message\":\"%s\",\"walk\":\"%s\"}",
                     agent_status_message(), walk_status_name(walk_status));
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
        int16_t itile_x, itile_y;

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
         * prisoner_2y/2+4 here, matching prop_offset+2's read-back). */
        raw_x = readword(fbuffer[OBJECTS], prop_offset + 4);
        raw_y = readword(fbuffer[OBJECTS], prop_offset + 2);
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
            "%s{\"name\":\"%s\",\"tile\":[%d,%d]}", first ? "" : ",",
            prop_name[item_id], (int)itile_x, (int)itile_y);
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
static int16_t walk_path_x[WALK_MAX_CELLS];
static int16_t walk_path_y[WALK_MAX_CELLS];
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

/* True when the walk's target is an exit/doorway cell (set at accept time
 * in handle_walk, from either an explicit {"exit":N} request or a
 * {"tile":[x,y]} landing on walk_grid_isexit[]) -- gates whether reaching
 * the target tile enters the CROSSING phase (part A) or ends the walk
 * immediately as arrived (plain-tile walks, unchanged Task 8 behavior). */
static bool walk_is_exit_target = false;

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
 * close enough to center. */
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
 *  - WALK_PHASE_SIDESTEP: entered on the walk's first stall while still
 *    en route to a non-final waypoint. Nudges perpendicular to the
 *    current leg's direction of travel for a bounded time, then re-BFS's
 *    from wherever that left the prisoner to the ORIGINAL target on a
 *    fresh grid snapshot, and resumes WALK_PHASE_PATH. One recovery per
 *    walk (walk_recovered latches after the first attempt); a second
 *    stall (or a stall on the final waypoint) is blocked immediately, as
 *    before Task 9.
 *
 * All three phases terminate exclusively through walk_cancel() (called
 * from walk_pump's shared per-tick preamble below), so the single
 * key-release choke point from Task 8 still holds for every new
 * termination path this adds. */
typedef enum { WALK_PHASE_PATH, WALK_PHASE_CROSS, WALK_PHASE_SIDESTEP } walk_phase_t;
static walk_phase_t walk_phase = WALK_PHASE_PATH;

/* One recovery attempt per walk (part B); reset in handle_walk. */
static bool walk_recovered = false;

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
 * other, each for a bounded time, before re-pathing. */
#define WALK_SIDESTEP_LEG_TICKS 25   /* ~0.4s per side */
static walk_dir_t walk_sidestep_dir[2];
static int walk_sidestep_idx = 0;
static int walk_sidestep_ticks = 0;

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

    room_x = saved_room_x; room_y = saved_room_y; offset = saved_offset;
    *out_width = width; *out_height = height;
    return found_exit ? WALK_SNAP_OK : WALK_SNAP_BAD_EXIT;
}

/* 4-connected BFS from (sx,sy) to (tx,ty) over walk_grid_walkable[] (already
 * populated by walk_snapshot_grid, width x height). On success, fills
 * walk_path_x/y[0..*out_len-1] with the waypoint tiles strictly after the
 * start tile through the target inclusive, and returns true. All working
 * arrays are function-local static (bounded at WALK_MAX_CELLS, matching
 * the grid's own cap) so this never mallocs and never touches the stack
 * for anything path-length-sized. */
static bool walk_bfs(uint16_t width, uint16_t height,
                     int16_t sx, int16_t sy, int16_t tx, int16_t ty,
                     int* out_len)
{
    static int16_t prev[WALK_MAX_CELLS];
    static int16_t queue[WALK_MAX_CELLS];
    static bool visited[WALK_MAX_CELLS];
    static int16_t rev[WALK_MAX_CELLS];
    static const int8_t dxs[4] = { 1, -1, 0, 0 };
    static const int8_t dys[4] = { 0, 0, 1, -1 };
    int qh = 0, qt = 0, i;
    int start = sy*(int)width + sx, target = ty*(int)width + tx;

    if (!walk_grid_walkable[start] || !walk_grid_walkable[target])
        return false;

    memset(visited, 0, (size_t)width * (size_t)height * sizeof(bool));
    visited[start] = true;
    prev[start] = -1;
    queue[qt++] = (int16_t)start;

    while (qh < qt) {
        int u = queue[qh++];
        int ux = u % (int)width, uy = u / (int)width;
        if (u == target) break;
        for (i = 0; i < 4; i++) {
            int nx = ux + dxs[i], ny = uy + dys[i], v;
            if (nx < 0 || ny < 0 || nx >= (int)width || ny >= (int)height)
                continue;
            v = ny*(int)width + nx;
            if (!walk_grid_walkable[v] || visited[v]) continue;
            visited[v] = true;
            prev[v] = (int16_t)u;
            queue[qt++] = (int16_t)v;
        }
    }

    if (!visited[target]) return false;

    {
        int len = 0, cur = target;
        while (cur != start) {
            rev[len++] = (int16_t)cur;
            cur = prev[cur];
        }
        for (i = 0; i < len; i++) {
            int cell = rev[len-1-i];
            walk_path_x[i] = (int16_t)(cell % (int)width);
            walk_path_y[i] = (int16_t)(cell / (int)width);
        }
        *out_len = len;
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
        if      (tx - fx ==  1) { arrival = WALK_DIR_RIGHT; have_arrival = true; }
        else if (tx - fx == -1) { arrival = WALK_DIR_LEFT;  have_arrival = true; }
        else if (ty - fy ==  1) { arrival = WALK_DIR_DOWN;  have_arrival = true; }
        else if (ty - fy == -1) { arrival = WALK_DIR_UP;    have_arrival = true; }
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
                walk_cancel(WALK_BLOCKED);
                return;
            }
        }
    }

    d   = walk_cross_cand[walk_cross_idx];
    px  = guybrush[current_nation].px;
    p2y = guybrush[current_nation].p2y;
    cx  = (int16_t)(walk_target_x * 32 + 16);
    cy  = (int16_t)(walk_target_y * 32 + 16);

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

/* SIDESTEP (recovery, part B) phase: hold one perpendicular side, then
 * the other, each for WALK_SIDESTEP_LEG_TICKS ticks; once both are tried,
 * release and re-BFS from wherever that left the prisoner to the walk's
 * ORIGINAL target (walk_target_x/y, not whatever waypoint we'd been
 * chasing) on a FRESH grid snapshot, then resume WALK_PHASE_PATH. Blocked
 * if the fresh snapshot or the re-BFS fails (e.g. the sidestep itself ran
 * into a wall and made no progress). */
static void walk_pump_sidestep(void)
{
    uint16_t width = 0, height = 0;
    int16_t sx, sy;
    int new_len = 0, snap;

    if (++walk_sidestep_ticks < WALK_SIDESTEP_LEG_TICKS)
        return;

    walk_sidestep_idx++;
    if (walk_sidestep_idx < 2) {
        walk_sidestep_ticks = 0;
        walk_hold_only(walk_dir_key(walk_sidestep_dir[walk_sidestep_idx]));
        return;
    }

    walk_release_keys();
    snap = walk_snapshot_grid(&width, &height, -1, NULL, NULL);
    if (snap != WALK_SNAP_OK) { walk_cancel(WALK_BLOCKED); return; }
    walk_width = width;
    walk_height = height;

    sx = guybrush[current_nation].px / 32;
    sy = guybrush[current_nation].p2y / 32;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int16_t)width)  sx = (int16_t)width  - 1;
    if (sy >= (int16_t)height) sy = (int16_t)height - 1;

    if (walk_target_x < 0 || walk_target_y < 0 ||
        walk_target_x >= (int16_t)width || walk_target_y >= (int16_t)height ||
        !walk_bfs(width, height, sx, sy, walk_target_x, walk_target_y, &new_len)) {
        walk_cancel(WALK_BLOCKED);
        return;
    }

    walk_path_len = new_len;
    walk_path_idx = 0;
    walk_stall_ticks = 0;
    walk_stall_px = guybrush[current_nation].px;
    walk_stall_p2y = guybrush[current_nation].p2y;
    walk_phase = WALK_PHASE_PATH;
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
     * blocked, per the brief. */
    if (current_nation != walk_nation) { walk_cancel(WALK_BLOCKED); return; }

    /* Whole-walk hard cap (part B): bounds path-following + one
     * sidestep-recovery attempt + crossing all taking their maximum time
     * in sequence, protecting against any pathological loop. */
    if (++walk_total_ticks > WALK_MAX_TICKS) { walk_cancel(WALK_BLOCKED); return; }

    /* Room change is success on every phase -- the walk got the prisoner
     * out of the room. This doubles as the CROSSING phase's actual
     * success signal (see its header comment): reaching it there needs no
     * phase-specific check of its own. */
    if (guybrush[current_nation].room != walk_room) { walk_cancel(WALK_ARRIVED); return; }

    if (walk_phase == WALK_PHASE_CROSS)    { walk_pump_cross();    return; }
    if (walk_phase == WALK_PHASE_SIDESTEP) { walk_pump_sidestep(); return; }

    /* WALK_PHASE_PATH: follow the BFS path (Task 8 behavior), plus the
     * stall -> one-shot sidestep-recovery hook (part B) and the
     * target-reached -> CROSSING-phase handoff (part A). */
    px  = guybrush[current_nation].px;
    p2y = guybrush[current_nation].p2y;
    tile_x = px / 32;
    tile_y = p2y / 32;

    if (px == walk_stall_px && p2y == walk_stall_p2y) {
        if (++walk_stall_ticks >= WALK_STALL_LIMIT) {
            if (!walk_recovered && walk_path_idx < walk_path_len - 1) {
                /* Non-final-waypoint stall, recovery not used yet:
                 * sidestep perpendicular to the current leg's direction
                 * of travel. Each BFS step is single-axis (walk_bfs is
                 * 4-connected), so comparing the waypoint we were heading
                 * for against our current tile unambiguously tells us
                 * which axis was "primary" (blocked) and which is
                 * perpendicular (the way around). */
                int16_t wtx = walk_path_x[walk_path_idx];
                walk_recovered = true;
                walk_phase = WALK_PHASE_SIDESTEP;
                if (wtx != tile_x) {
                    walk_sidestep_dir[0] = WALK_DIR_DOWN;
                    walk_sidestep_dir[1] = WALK_DIR_UP;
                } else {
                    walk_sidestep_dir[0] = WALK_DIR_RIGHT;
                    walk_sidestep_dir[1] = WALK_DIR_LEFT;
                }
                walk_sidestep_idx = 0;
                walk_sidestep_ticks = 0;
                walk_hold_only(walk_dir_key(walk_sidestep_dir[0]));
                walk_stall_ticks = 0;
                return;
            }
            walk_cancel(WALK_BLOCKED);
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
        walk_cancel(WALK_ARRIVED);
        return;
    }

    target_x = walk_path_x[walk_path_idx];
    target_y = walk_path_y[walk_path_idx];
    cx = (int16_t)(target_x * 32 + 16);
    cy = (int16_t)(target_y * 32 + 16);

    if (px < cx - WALK_DEADBAND) {
        key_down[walk_key_right] = true;
        key_down[walk_key_left] = false;  key_readonce[walk_key_left] = false;
    } else if (px > cx + WALK_DEADBAND) {
        key_down[walk_key_left] = true;
        key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
    } else {
        key_down[walk_key_left] = false;  key_readonce[walk_key_left] = false;
        key_down[walk_key_right] = false; key_readonce[walk_key_right] = false;
    }

    if (p2y < cy - WALK_DEADBAND) {
        key_down[walk_key_down] = true;
        key_down[walk_key_up] = false;    key_readonce[walk_key_up] = false;
    } else if (p2y > cy + WALK_DEADBAND) {
        key_down[walk_key_up] = true;
        key_down[walk_key_down] = false;  key_readonce[walk_key_down] = false;
    } else {
        key_down[walk_key_up] = false;    key_readonce[walk_key_up] = false;
        key_down[walk_key_down] = false;  key_readonce[walk_key_down] = false;
    }
}

/* POST /walk {"tile":[x,y]} | {"exit":N} | {"cancel":true}. See docs/
 * AGENT-API.md for the full contract. Any /input request that arrives
 * while a walk is in progress cancels the walk first (handle_input, per
 * the brief); any /walk request that arrives while the input queue is
 * busy is rejected with 409 rather than interleaving the two key-holding
 * mechanisms. */
static void handle_walk(int cfd, const char* body)
{
    long a = 0, b = 0, exit_idx;
    uint16_t width = 0, height = 0;
    int16_t sx, sy, tx = 0, ty = 0;
    int snap, path_len = 0;
    bool is_exit_target;
    char resp[80]; int n;

    if (body) {
        const char* p = strstr(body, "\"cancel\"");
        if (p && strstr(p, "true")) {
            walk_cancel(WALK_IDLE);
            send_response(cfd, 200, "application/json", "{\"walking\":false}", 18);
            return;
        }
    }

    if (agent_input_queue_depth() > 0) {
        static const char* busy = "{\"error\":\"input busy\"}";
        send_response(cfd, 409, "application/json", busy, strlen(busy));
        return;
    }

    exit_idx = body ? json_int(body, "exit", -1) : -1;
    if (exit_idx >= 0) {
        snap = walk_snapshot_grid(&width, &height, (int)exit_idx, &tx, &ty);
    } else if (body && json_intpair(body, "tile", &a, &b)) {
        snap = walk_snapshot_grid(&width, &height, -1, NULL, NULL);
        tx = (int16_t)a; ty = (int16_t)b;
    } else {
        static const char* need = "expected \"tile\":[x,y] or \"exit\":N";
        send_response(cfd, 400, "text/plain", need, strlen(need));
        return;
    }

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
    /* walk_grid_walkable already folds in exit cells (readexit & 0x1F) even
     * when the underlying tile id is 0 -- see walk_snapshot_grid -- so an
     * {"exit":N} target that lands on a void tile is still accepted here,
     * matching /room's 'E' overlay on void cells. */
    if (!walk_grid_walkable[(int)ty*(int)width + tx]) {
        static const char* voidtile = "target is void tile";
        send_response(cfd, 400, "text/plain", voidtile, strlen(voidtile));
        return;
    }

    /* Part A: an explicit {"exit":N} request is always an exit walk; a
     * {"tile":[x,y]} request is one too if it happens to land on an exit
     * cell (walk_grid_isexit[], populated by the same scan that just
     * built walk_grid_walkable[] above) -- gates the target-reached ->
     * CROSSING-phase handoff in walk_pump. */
    is_exit_target = (exit_idx >= 0) || walk_grid_isexit[(int)ty*(int)width + tx];

    sx = guybrush[current_nation].px / 32;
    sy = guybrush[current_nation].p2y / 32;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int16_t)width)  sx = (int16_t)width  - 1;
    if (sy >= (int16_t)height) sy = (int16_t)height - 1;

    if (!walk_bfs(width, height, sx, sy, tx, ty, &path_len)) {
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
    walk_start_x = sx;
    walk_start_y = sy;
    walk_is_exit_target = is_exit_target;
    walk_phase = WALK_PHASE_PATH;
    walk_recovered = false;
    walk_total_ticks = 0;
    walk_status = WALK_WALKING;

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

void agent_api_tick(void)
{
    struct timeval tv = { 0, 200000 };  // 200 ms cap per request
    int cfd;
    input_pump();   /* first: held keys release on schedule even with no
                     * client connected (and even if listen_fd is closed). */
    walk_pump();    /* right after input_pump: drives walk-held direction
                     * keys on the same per-tick cadence, independent of
                     * whether a client is connected this tick. */
    if (listen_fd < 0) return;
    cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) return;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    handle_request(cfd);
    close(cfd);
}
#endif
