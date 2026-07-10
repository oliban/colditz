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
        if (!enqueue_key(KEY_PAUSE, 100)) {
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
    n = json_append(json, sizeof(json), n, "],\"message\":\"%s\"}",
                     agent_status_message());
    send_response(cfd, 200, "application/json", json, (size_t)n);
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
    } else
        send_response(cfd, 404, "text/plain", "not found", 9);
}

void agent_api_tick(void)
{
    struct timeval tv = { 0, 200000 };  // 200 ms cap per request
    int cfd;
    input_pump();   /* first: held keys release on schedule even with no
                     * client connected (and even if listen_fd is closed). */
    if (listen_fd < 0) return;
    cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) return;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    handle_request(cfd);
    close(cfd);
}
#endif
