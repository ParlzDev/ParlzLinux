/* http_parser.c - request parsing + response head construction */
#include "pweb.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int reason_str(int status, char *out, size_t n)
{
    const char *r = "Unknown";
    switch (status) {
    case 200: r = "OK"; break;
    case 201: r = "Created"; break;
    case 204: r = "No Content"; break;
    case 301: r = "Moved Permanently"; break;
    case 302: r = "Found"; break;
    case 304: r = "Not Modified"; break;
    case 400: r = "Bad Request"; break;
    case 401: r = "Unauthorized"; break;
    case 403: r = "Forbidden"; break;
    case 404: r = "Not Found"; break;
    case 405: r = "Method Not Allowed"; break;
    case 408: r = "Request Timeout"; break;
    case 413: r = "Request Entity Too Large"; break;
    case 414: r = "URI Too Long"; break;
    case 421: r = "Misdirected Request"; break;
    case 451: r = "Unavailable For Legal Reasons"; break;
    case 500: r = "Internal Server Error"; break;
    case 501: r = "Not Implemented"; break;
    case 502: r = "Bad Gateway"; break;
    case 503: r = "Service Unavailable"; break;
    case 504: r = "Gateway Timeout"; break;
    default:  r = "Unknown"; break;
    }
    snprintf(out, n, "%s", r);
    return 0;
}

/* parse the request head. Returns 0 ok, -1 fatal.
 * Advances *pos past the head; leaves the head NUL-terminated in buf.
 * body bytes may still be pending in buf at [pos, pos+remaining). */
int parse_request(req_t *req, char *buf, int *pos, int buflen)
{
    memset(req, 0, sizeof(*req));
    req->content_length = -1;
    char *p = buf + *pos;
    char *end = buf + buflen;

    /* find end of request head: he = just past the terminating empty
     * line ("\r\n" that follows the last header). A request whose body
     * already arrived in the same recv chunk therefore terminates at
     * that empty line, not at the end of the chunk. */
    char *he = NULL;
    {
        char *scan = p;
        while (scan < end) {
            char *ln = (char*)memchr(scan, '\n', (size_t)(end - scan));
            if (!ln) { break; }
            /* An empty line ("\r\n" or bare "\n") terminates the head. */
            int empty = 1;
            char *q = scan;
            while (q < ln) {
                if (*q != '\r' && *q != '\n') { empty = 0; break; }
                q++;
            }
            if (empty) {
                he = ln + 1;          /* just past the final \n */
                break;
            }
            scan = ln + 1;
        }
        if (!he) { return -1; }
    }

    /* request line: METHOD SP TARGET SP VERSION */
    char *sp1 = strchr(p, ' ');
    if (!sp1 || sp1 >= he) { return -1; }
    {
        size_t ml = (size_t)(sp1 - p);
        if (ml >= sizeof(req->method)) { return -1; }
        memcpy(req->method, p, ml);
        req->method[ml] = 0;
        p = sp1 + 1;
        char *sp2 = strchr(p, ' ');
        if (!sp2 || sp2 >= he) { return -1; }
        size_t tl = (size_t)(sp2 - p);
        if (tl >= sizeof(req->target)) { return -1; }
        memcpy(req->target, p, tl);
        req->target[tl] = 0;
        p = sp2 + 1;
        /* request-line version, terminated by the first \n within the
         * head (a trailing \r is part of the CRLF). */
        char *le = (char*)memchr(p, '\n', he - p);
        if (!le) { le = he; }
        size_t vlen = (size_t)(le - p);
        while (vlen > 0 && p[vlen - 1] == '\r') { vlen--; }
        if (vlen >= 12 || strncmp(p, "HTTP/", 5) != 0) { return -1; }
        const char *vn = p + 5;
        size_t vnl = vlen - 5;
        if (vnl >= sizeof(req->version)) { return -1; }
        memcpy(req->version, vn, vnl);
        req->version[vnl] = 0;
        if (strcmp(req->version, "1.0") != 0 &&
            strcmp(req->version, "1.1") != 0) {
            return -1;
        }
        /* headers start just after the request line */
        p = le + 1;
    }

    /* headers: walk lines between p and he. Each header line ends with
     * CRLF (or a bare LF). The loop bound he is just past the empty
     * terminator line, so header content lies strictly before he. */
    char *hp = p;
    while (hp < he) {
        /* end of this header line: the \n (or bare \n) before the next */
        char *le = (char*)memchr(hp, '\n', (size_t)(he - hp));
        char *line_end;
        if (le) {
            line_end = le;            /* points at the \n */
        } else {
            line_end = he - 1;       /* last byte before the empty line */
        }
        /* NUL-terminate the line at its terminator (\n, or the \r in a
         * CRLF pair) so key/value strings are bounded. */
        char *nul_at = line_end;
        if (nul_at > hp && nul_at[-1] == '\r') { nul_at = nul_at - 1; }
        char save = *nul_at;
        *nul_at = 0;
        char *c = strchr(hp, ':');
        if (c) {
            size_t kl = (size_t)(c - hp);
            if (kl < 64) {
                char k[64], v[512];
                memcpy(k, hp, kl); k[kl] = 0;
                c++;
                while (*c == ' ' || *c == '\t') { c++; }
                size_t vl = strlen(c);
                if (vl >= 512) { vl = 511; }
                memcpy(v, c, vl);
                v[vl] = 0;
                if (req->n_hdrs < 48) {
                    strcpy(req->hdrs[req->n_hdrs].k, k);
                    strcpy(req->hdrs[req->n_hdrs].v, v);
                    req->n_hdrs++;
                }
                if (strcasecmp(k, "Content-Length") == 0) {
                    req->content_length = atoi(v);
                }
                if (strcasecmp(k, "Transfer-Encoding") == 0 &&
                    casestrs(v, "chunked")) {
                    req->is_chunked = 1;
                }
            }
        }
        *nul_at = save;
        /* advance past the full terminator (\r\n or \n) */
        hp = line_end + 1;
    }
    *pos = (int)(he - buf);
    return 0;
}

const char *req_hdr(const req_t *r, const char *k)
{
    for (int i = 0; i < r->n_hdrs; i++) {
        if (strcasecmp(r->hdrs[i].k, k) == 0) { return r->hdrs[i].v; }
    }
    return NULL;
}

/* read the request body (Content-Length bytes) into a malloc'd NUL-terminated
 * buffer. Returns NULL if oversize or on error. */
char *read_body(int fd, int content_length, long max_body, int *cl_out)
{
    if (cl_out) { *cl_out = content_length; }
    if (content_length <= 0) { return NULL; }
    if ((long)content_length > max_body) { return NULL; }
    char *body = (char*)malloc(content_length + 1);
    if (!body) { return NULL; }
    int got = 0;
    while (got < content_length) {
        ssize_t r = recv(fd, body + got, (size_t)(content_length - got), 0);
        if (r <= 0) { free(body); return NULL; }
        got += (int)r;
    }
    body[content_length] = 0;
    return body;
}

/* sniff an HTTP response head from a byte stream (used by proxy/CGL).
 * Returns 0 when the head is complete, -1 when more bytes are needed. */
int parse_http_response_head(char *buf, size_t len, int *status,
                             int *content_length, int *chunked,
                             size_t *head_end)
{
    *content_length = -1;
    *chunked = 0;
    char *p = buf;
    char *end = buf + len;
    char *nl = (char*)memchr(p, '\n', len);
    if (!nl) { return -1; }
    char save = *nl;
    *nl = 0;
    if (strncmp(p, "HTTP/", 5) != 0) { *nl = save; return -1; }
    *status = atoi(p + 8);
    *nl = save;
    p = nl + 1;
    if (*p == '\r') { p++; }
    while (p < end) {
        nl = (char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) { return -1; }
        save = *nl;
        *nl = 0;
        char *nxt = nl + 1;
        if (*nxt == '\r') { nxt++; }
        if (p == nxt) {
            *nl = save;
            if (head_end) { *head_end = nxt - buf; }
            return 0;
        }
        char *c = strchr(p, ':');
        if (c) {
            size_t kl = (size_t)(c - p);
            if (kl < 32) {
                char k[32];
                memcpy(k, p, kl); k[kl] = 0;
                c++;
                while (*c == ' ' || *c == '\t') { c++; }
                if (strcasecmp(k, "Content-Length") == 0) {
                    *content_length = atoi(c);
                }
                if (strcasecmp(k, "Transfer-Encoding") == 0 &&
                    casestrs(c, "chunked")) {
                    *chunked = 1;
                }
            }
        }
        *nl = save;
        p = nxt;
    }
    return -1;
}
