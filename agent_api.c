#if !defined(WIN32) && !defined(PSP)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include "agent_api.h"

bool agent_api_enabled = false;
static int listen_fd = -1;

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

static void send_response(int fd, int code, const char* ctype,
                          const void* body, size_t len)
{
    char hdr[256];
    const char* msg = (code==200)?"OK":(code==400)?"Bad Request":
                      (code==404)?"Not Found":"Error";
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        code, msg, ctype, len);
    write(fd, hdr, n);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, (const char*)body + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void handle_state(int fd)
{
    const char* json = "{\"ok\":true}";
    send_response(fd, 200, "application/json", json, strlen(json));
}

static void handle_request(int fd)
{
    static char req_buf[4096];
    char method[8] = "", path[64] = "";
    ssize_t n = recv(fd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) return;
    req_buf[n] = '\0';
    if (sscanf(req_buf, "%7s %63s", method, path) != 2) {
        send_response(fd, 400, "text/plain", "bad request", 11);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/state"))
        handle_state(fd);
    else
        send_response(fd, 404, "text/plain", "not found", 9);
}

void agent_api_tick(void)
{
    struct timeval tv = { 0, 200000 };  // 200 ms cap per request
    int fd;
    if (listen_fd < 0) return;
    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    handle_request(fd);
    close(fd);
}
#endif
