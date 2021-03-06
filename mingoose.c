#include "mingoose.h"
//-- src/util.c --

// Return fake connection structure. Used for logging, if connection
// is not applicable at the moment of logging.
struct mg_connection *create_fake_connection(struct mg_context *ctx) {
    static struct mg_connection fake_connection;
    fake_connection.ctx = ctx;
    // See https://github.com/cesanta/mongoose/issues/236
    fake_connection.event.user_data = ctx->user_data;
    return &fake_connection;
}
//-- end of src/util.c --
//-- src/string.c --
//-- end of src/string.c --

//-- src/unix.c --

int mg_stat(const char *path, struct file *filep) {
    struct stat st;

    filep->modification_time = (time_t) 0;
    if (stat(path, &st) == 0) {
        filep->size = st.st_size;
        filep->modification_time = st.st_mtime;
        filep->is_directory = S_ISDIR(st.st_mode);

        // See https://github.com/cesanta/mongoose/issues/109
        // Some filesystems report modification time as 0. Artificially
        // bump it up to mark mg_stat() success.
        if (filep->modification_time == (time_t) 0) {
            filep->modification_time = (time_t) 1;
        }
    }

    return filep->modification_time != (time_t) 0;
}

static void set_close_on_exec(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int mg_start_thread(mg_thread_func_t func, void *param) {
    pthread_t thread_id;
    pthread_attr_t attr;
    int result;

    (void) pthread_attr_init(&attr);
    (void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

#if USE_STACK_SIZE > 1
    // Compile-time option to control stack size, e.g. -DUSE_STACK_SIZE=16384
    (void) pthread_attr_setstacksize(&attr, USE_STACK_SIZE);
#endif

    result = pthread_create(&thread_id, &attr, func, param);
    pthread_attr_destroy(&attr);

    return result;
}


static int set_non_blocking_mode(SOCKET sock) {
    int flags;

    flags = fcntl(sock, F_GETFL, 0);
    (void) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    return 0;
}
//-- end of src/unix.c --
//-- src/mingoose.c --

// Return number of bytes left to read for this connection
int64_t left_to_read(const struct mg_connection *conn) {
    return conn->content_len + conn->request_len - conn->num_bytes_read;
}

int call_user(int type, struct mg_connection *conn, void *p) {
    if (conn != NULL && conn->ctx != NULL) {
        conn->event.user_data = conn->ctx->user_data;
        conn->event.type = type;
        conn->event.event_param = p;
        conn->event.request_info = &conn->request_info;
        conn->event.conn = conn;
    }
    return conn == NULL || conn->ctx == NULL || conn->ctx->event_handler == NULL ?
        0 : conn->ctx->event_handler(&conn->event);
}

void sockaddr_to_string(char *buf, size_t len,
                        const union usa *usa) {
    buf[0] = '\0';
    inet_ntop(usa->sa.sa_family, (void *) &usa->sin.sin_addr, buf, len);
}


const char *mg_version(void) {
    return MONGOOSE_VERSION;
}

// HTTP 1.1 assumes keep alive if "Connection:" header is not set
// This function must tolerate situations when connection info is not
// set up, for example if request parsing failed.
static int should_keep_alive(const struct mg_connection *conn) {
    const char *http_version = conn->request_info.http_version;
    const char *header = mg_get_header(conn, "Connection");
    if (conn->must_close ||
        conn->status_code == 401 ||
        mg_strcasecmp(conn->ctx->config[op("enable_keep_alive")], "yes") != 0 ||
        (header != NULL && mg_strcasecmp(header, "keep-alive") != 0) ||
        (header == NULL && http_version && strcmp(http_version, "1.1"))) {
        return 0;
    }
    return 1;
}

const char *suggest_connection_header(const struct mg_connection *conn) {
    return should_keep_alive(conn) ? "keep-alive" : "close";
}


// Write data to the IO channel - opened file descriptor, socket or SSL
// descriptor. Return number of bytes written.
int64_t push(FILE *fp, SOCKET sock, SSL *ssl, const char *buf,
                    int64_t len) {
    int64_t sent;
    int n, k;

    (void) ssl;  // Get rid of warning
    sent = 0;
    while (sent < len) {

        // How many bytes we send in this iteration
        k = len - sent > INT_MAX ? INT_MAX : (int) (len - sent);

        if (fp != NULL) {
            n = (int) fwrite(buf + sent, 1, (size_t) k, fp);
            if (ferror(fp))
                n = -1;
        } else {
            n = send(sock, buf + sent, (size_t) k, MSG_NOSIGNAL);
        }

        if (n <= 0)
            break;

        sent += n;
    }

    return sent;
}

// Read from IO channel - opened file descriptor, socket, or SSL descriptor.
// Return negative value on error, or number of bytes read on success.
int pull(FILE *fp, struct mg_connection *conn, char *buf, int len) {
    int nread;

    if (len <= 0) return 0;
    if (fp != NULL) {
        // Use read() instead of fread(), because if we're reading from the CGI
        // pipe, fread() may block until IO buffer is filled up. We cannot afford
        // to block and must pass all read bytes immediately to the client.
        nread = read(fileno(fp), buf, (size_t) len);
    } else {
        nread = recv(conn->client.sock, buf, (size_t) len, 0);
    }
    if (nread > 0) {
        conn->num_bytes_read += nread;
    }

    return conn->ctx->stop_flag ? -1 : nread;
}


int mg_write(struct mg_connection *conn, const void *buf, int len) {
    time_t now;
    int64_t n, total, allowed;

    if (conn->throttle > 0) {
        if ((now = time(NULL)) != conn->last_throttle_time) {
            conn->last_throttle_time = now;
            conn->last_throttle_bytes = 0;
        }
        allowed = conn->throttle - conn->last_throttle_bytes;
        if (allowed > (int64_t) len) {
            allowed = len;
        }
        if ((total = push(NULL, conn->client.sock, conn->ssl, (const char *) buf,
                          (int64_t) allowed)) == allowed) {
            buf = (char *) buf + total;
            conn->last_throttle_bytes += total;
            while (total < (int64_t) len && conn->ctx->stop_flag == 0) {
                allowed = conn->throttle > (int64_t) len - total ?
                    (int64_t) len - total : conn->throttle;
                if ((n = push(NULL, conn->client.sock, conn->ssl, (const char *) buf,
                              (int64_t) allowed)) != allowed) {
                    break;
                }
                sleep(1);
                conn->last_throttle_bytes = allowed;
                conn->last_throttle_time = time(NULL);
                buf = (char *) buf + n;
                total += n;
            }
        }
    } else {
        total = push(NULL, conn->client.sock, conn->ssl, (const char *) buf,
                     (int64_t) len);
    }
    return (int) total;
}

int mg_url_decode(const char *src, int src_len, char *dst,
                  int dst_len, int is_form_url_encoded) {
    int i, j, a, b;
#define HEXTOI(x) (isdigit(x) ? x - '0' : x - 'W')

    for (i = j = 0; i < src_len && j < dst_len - 1; i++, j++) {
        if (src[i] == '%' && i < src_len - 2 &&
            isxdigit(* (const unsigned char *) (src + i + 1)) &&
            isxdigit(* (const unsigned char *) (src + i + 2))) {
            a = tolower(* (const unsigned char *) (src + i + 1));
            b = tolower(* (const unsigned char *) (src + i + 2));
            dst[j] = (char) ((HEXTOI(a) << 4) | HEXTOI(b));
            i += 2;
        } else if (is_form_url_encoded && src[i] == '+') {
            dst[j] = ' ';
        } else {
            dst[j] = src[i];
        }
    }

    dst[j] = '\0'; // Null-terminate the destination

    return i >= src_len ? j : -1;
}

int mg_get_var(const char *data, size_t data_len, const char *name,
               char *dst, size_t dst_len) {
    const char *p, *e, *s;
    size_t name_len;
    int len;

    if (dst == NULL || dst_len == 0) {
        len = -2;
    } else if (data == NULL || name == NULL || data_len == 0) {
        len = -1;
        dst[0] = '\0';
    } else {
        name_len = strlen(name);
        e = data + data_len;
        len = -1;
        dst[0] = '\0';

        // data is "var1=val1&var2=val2...". Find variable first
        for (p = data; p + name_len < e; p++) {
            if ((p == data || p[-1] == '&') && p[name_len] == '=' &&
                !mg_strncasecmp(name, p, name_len)) {

                // Point p to variable value
                p += name_len + 1;

                // Point s to the end of the value
                s = (const char *) memchr(p, '&', (size_t)(e - p));
                if (s == NULL) {
                    s = e;
                }
                assert(s >= p);

                // Decode variable into destination buffer
                len = mg_url_decode(p, (size_t)(s - p), dst, dst_len, 1);

                // Redirect error code from -1 to -2 (destination buffer too small).
                if (len == -1) {
                    len = -2;
                }
                break;
            }
        }
    }

    return len;
}

int mg_get_cookie(const char *cookie_header, const char *var_name,
                  char *dst, size_t dst_size) {
    const char *s, *p, *end;
    int name_len, len = -1;

    if (dst == NULL || dst_size == 0) {
        len = -2;
    } else if (var_name == NULL || (s = cookie_header) == NULL) {
        len = -1;
        dst[0] = '\0';
    } else {
        name_len = (int) strlen(var_name);
        end = s + strlen(s);
        dst[0] = '\0';

        for (; (s = mg_strcasestr(s, var_name)) != NULL; s += name_len) {
            if (s[name_len] == '=') {
                s += name_len + 1;
                if ((p = strchr(s, ' ')) == NULL)
                    p = end;
                if (p[-1] == ';')
                    p--;
                if (*s == '"' && p[-1] == '"' && p > s + 1) {
                    s++;
                    p--;
                }
                if ((size_t) (p - s) < dst_size) {
                    len = p - s;
                    mg_strlcpy(dst, s, (size_t) len + 1);
                } else {
                    len = -3;
                }
                break;
            }
        }
    }
    return len;
}


void mg_url_encode(const char *src, char *dst, size_t dst_len) {
    static const char *dont_escape = "._-$,;~()";
    static const char *hex = "0123456789abcdef";
    const char *end = dst + dst_len - 1;

    for (; *src != '\0' && dst < end; src++, dst++) {
        if (isalnum(*(const unsigned char *) src) ||
            strchr(dont_escape, * (const unsigned char *) src) != NULL) {
            *dst = *src;
        } else if (dst + 2 < end) {
            dst[0] = '%';
            dst[1] = hex[(* (const unsigned char *) src) >> 4];
            dst[2] = hex[(* (const unsigned char *) src) & 0xf];
            dst += 2;
        }
    }

    *dst = '\0';
}


int must_hide_file(struct mg_connection *conn, const char *path) {
    const char *pw_pattern = "**" PASSWORDS_FILE_NAME "$";
    const char *pattern = conn->ctx->config[op("hide_files_patterns")];
    return match_prefix(pw_pattern, strlen(pw_pattern), path) > 0 ||
        (pattern != NULL && match_prefix(pattern, strlen(pattern), path) > 0);
}



int parse_range_header(const char *header, int64_t *a, int64_t *b) {
    return sscanf(header, "bytes=%" INT64_FMT "-%" INT64_FMT, a, b);
}

void gmt_time_string(char *buf, size_t buf_len, time_t *t) {
    strftime(buf, buf_len, "%a, %d %b %Y %H:%M:%S GMT", gmtime(t));
}

void construct_etag(char *buf, size_t buf_len,
                           const struct file *filep) {
    snprintf(buf, buf_len, "\"%lx.%" INT64_FMT "\"",
             (unsigned long) filep->modification_time, filep->size);
}

void fclose_on_exec(FILE *fp) {
    if (fp != NULL) {
        fcntl(fileno(fp), F_SETFD, FD_CLOEXEC);
    }
}

FILE *mg_upload(struct mg_connection *conn, const char *destination_dir,
                char *path, int path_len) {
    const char *content_type_header, *boundary_start;
    char *buf, fname[1024], boundary[100], *s;
    int bl, n, i, j, headers_len, boundary_len, eof, buf_len, to_read, len = 0;
    FILE *fp;

    // Request looks like this:
    //
    // POST /upload HTTP/1.1
    // Host: 127.0.0.1:8080
    // Content-Length: 244894
    // Content-Type: multipart/form-data; boundary=----WebKitFormBoundaryRVr
    //
    // ------WebKitFormBoundaryRVr
    // Content-Disposition: form-data; name="file"; filename="accum.png"
    // Content-Type: image/png
    //
    //  <89>PNG
    //  <PNG DATA>
    // ------WebKitFormBoundaryRVr

    // Extract boundary string from the Content-Type header
    if ((content_type_header = mg_get_header(conn, "Content-Type")) == NULL ||
        (boundary_start = mg_strcasestr(content_type_header,
                                        "boundary=")) == NULL ||
        (sscanf(boundary_start, "boundary=\"%99[^\"]\"", boundary) == 0 &&
         sscanf(boundary_start, "boundary=%99s", boundary) == 0) ||
        boundary[0] == '\0') {
        return NULL;
    }

    boundary_len = strlen(boundary);
    bl = boundary_len + 4;  // \r\n--<boundary>

    //                     buf
    // conn->buf            |<--------- buf_len ------>|
    //    |=================|==========|===============|
    //    |<--request_len-->|<--len--->|               |
    //    |<-----------data_len------->|      conn->buf + conn->buf_size

    buf = conn->buf + conn->request_len;
    buf_len = conn->buf_size - conn->request_len;
    len = conn->data_len - conn->request_len;

    for (;;) {
        // Pull in headers
        assert(len >= 0 && len <= buf_len);
        to_read = buf_len - len;
        if (to_read > left_to_read(conn)) {
            to_read = (int) left_to_read(conn);
        }
        while (len < buf_len &&
               (n = pull(NULL, conn, buf + len, to_read)) > 0) {
            len += n;
        }
        if ((headers_len = get_request_len(buf, len)) <= 0) {
            break;
        }

        // Fetch file name.
        fname[0] = '\0';
        for (i = j = 0; i < headers_len; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n') {
                buf[i] = buf[i + 1] = '\0';
                // TODO(lsm): don't expect filename to be the 3rd field,
                // parse the header properly instead.
                sscanf(&buf[j], "Content-Disposition: %*s %*s filename=\"%1023[^\"]",
                       fname);
                j = i + 2;
            }
        }

        // Give up if the headers are not what we expect
        if (fname[0] == '\0') {
            break;
        }

        // Move data to the beginning of the buffer
        assert(len >= headers_len);
        memmove(buf, &buf[headers_len], len - headers_len);
        len -= headers_len;
        conn->data_len = conn->request_len + len;

        // We open the file with exclusive lock held. This guarantee us
        // there is no other thread can save into the same file simultaneously.
        fp = NULL;

        // Construct destination file name. Do not allow paths to have slashes.
        if ((s = strrchr(fname, '/')) == NULL &&
            (s = strrchr(fname, '\\')) == NULL) {
            s = fname;
        }

        // Open file in binary mode. TODO: set an exclusive lock.
        snprintf(path, path_len, "%s/%s", destination_dir, s);
        if ((fp = fopen(path, "wb")) == NULL) {
            break;
        }

        // Read POST data, write into file until boundary is found.
        eof = n = 0;
        do {
            len += n;
            for (i = 0; i < len - bl; i++) {
                if (!memcmp(&buf[i], "\r\n--", 4) &&
                    !memcmp(&buf[i + 4], boundary, boundary_len)) {
                    // Found boundary, that's the end of file data.
                    fwrite(buf, 1, i, fp);
                    eof = 1;
                    memmove(buf, &buf[i + bl], len - (i + bl));
                    len -= i + bl;
                    break;
                }
            }
            if (!eof && len > bl) {
                fwrite(buf, 1, len - bl, fp);
                memmove(buf, &buf[len - bl], bl);
                len = bl;
            }
            to_read = buf_len - len;
            if (to_read > left_to_read(conn)) {
                to_read = (int) left_to_read(conn);
            }
        } while (!eof && (n = pull(NULL, conn, buf + len, to_read)) > 0);
        conn->data_len = conn->request_len + len;

        if (eof) {
            rewind(fp);
            return fp;
        } else {
            fclose(fp);
        }
    }

    return NULL;
}

static void close_all_listening_sockets(struct mg_context *ctx) {
    closesocket(ctx->listening_socket_fd);
}

static int is_valid_port(unsigned int port) {
    return port > 0 && port < 0xffff;
}

// Valid listening port specification is: [ip_address:]port[s]
// Examples: 80, 443s, 127.0.0.1:3128, 1.2.3.4:8080s
// TODO(lsm): add parsing of the IPv6 address
static int parse_port_string(const char *ptr, struct socket *so) {
    unsigned int a, b, c, d, ch, port;
    int len;

    // MacOS needs that. If we do not zero it, subsequent bind() will fail.
    // Also, all-zeroes in the socket address means binding to all addresses
    // for both IPv4 and IPv6 (INADDR_ANY and IN6ADDR_ANY_INIT).
    memset(so, 0, sizeof(*so));
    so->lsa.sin.sin_family = AF_INET;

    if (sscanf(ptr, "%u.%u.%u.%u:%u%n", &a, &b, &c, &d, &port, &len) == 5) {
        // Bind to a specific IPv4 address, e.g. 192.168.1.5:8080
        so->lsa.sin.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
        so->lsa.sin.sin_port = htons((uint16_t) port);
    } else if (sscanf(ptr, "%u%n", &port, &len) == 1) {
        // If only port is specified, bind to IPv4, INADDR_ANY
        so->lsa.sin.sin_port = htons((uint16_t) port);
    } else {
        port = len = 0;   // Parsing failure. Make port invalid.
    }

    ch = ptr[len];  // Next character after the port number
    // Make sure the port is valid and string ends with '\0'
    return is_valid_port(port) && (ch == '\0');
}

static int socket_bind_listen(struct mg_context *ctx) {
    int on = 1;
    struct socket so;

    if (!parse_port_string(ctx->settings.ports, &so)) {
        cry(create_fake_connection(ctx), "%s: %s: invalid port spec. Expecting : %s",
            __func__, ctx->settings.ports, "[IP_ADDRESS:]PORT");
        close_all_listening_sockets(ctx);
        return 0;
    }

    if ((so.sock = socket(so.lsa.sa.sa_family, SOCK_STREAM, 6)) ==
        INVALID_SOCKET ||
        // On Windows, SO_REUSEADDR is recommended only for
        // broadcast UDP sockets
        setsockopt(so.sock, SOL_SOCKET, SO_REUSEADDR,
                   (void *) &on, sizeof(on)) != 0 ||
        bind(so.sock, &so.lsa.sa, so.lsa.sa.sa_family == AF_INET ?
             sizeof(so.lsa.sin) : sizeof(so.lsa)) != 0 ||
        listen(so.sock, SOMAXCONN) != 0 ) {
        cry(create_fake_connection(ctx), "%s: cannot bind to %s: %d (%s)", __func__,
            ctx->settings.ports, ERRNO, strerror(errno));
        closesocket(so.sock);

        close_all_listening_sockets(ctx);
        return 0;
    }

    set_close_on_exec(so.sock);
    ctx->listening_socket_fd = so.sock;
    return 1;
}

/**
 * @return bool
 */
static int mg_setuid(struct mg_context *ctx) {
    struct passwd *pw;
    const char *username = ctx->config[op("run_as_user")];

    if (username == NULL) {
        return 1;
    }

    pw = getpwnam(username);

    if (pw == NULL) {
        cry(create_fake_connection(ctx), "%s: unknown user [%s]", __func__, username);
        return 0;
    }

    if (setgid(pw->pw_gid) == -1) {
        cry(create_fake_connection(ctx), "%s: setgid(%s): %s", __func__, username, strerror(errno));
        return 0;
    }

    if (setuid(pw->pw_uid) == -1) {
        cry(create_fake_connection(ctx), "%s: setuid(%s): %s", __func__, username, strerror(errno));
        return 0;
    }

    return 1;
}


static int check_globalpassfile(struct mg_context *ctx) {
    struct file file = STRUCT_FILE_INITIALIZER;
    const char *path = ctx->settings.global_passwords_file;
    if (path != NULL && !mg_stat(path, &file)) {
        cry(create_fake_connection(ctx), "Cannot open %s: %s", path, strerror(ERRNO));
        return 0;
    }
    return 1;
}

static void close_socket_gracefully(struct mg_connection *conn) {
    struct linger linger;

    // Set linger option to avoid socket hanging out after close. This prevent
    // ephemeral port exhaust problem under high QPS.
    linger.l_onoff = 1;
    linger.l_linger = 1;
    setsockopt(conn->client.sock, SOL_SOCKET, SO_LINGER,
               (char *) &linger, sizeof(linger));

    // Send FIN to the client
    shutdown(conn->client.sock, SHUT_WR);
    set_non_blocking_mode(conn->client.sock);

    // Now we know that our FIN is ACK-ed, safe to close
    closesocket(conn->client.sock);
}

static void close_connection(struct mg_connection *conn) {
    conn->must_close = 1;

    if (conn->client.sock != INVALID_SOCKET) {
        close_socket_gracefully(conn);
        conn->client.sock = INVALID_SOCKET;
    }
}

void mg_close_connection(struct mg_connection *conn) {
    close_connection(conn);
    free(conn);
}

static int is_valid_uri(const char *uri) {
    // Conform to http://www.w3.org/Protocols/rfc2616/rfc2616-sec5.html#sec5.1.2
    // URI can be an asterisk (*) or should start with slash.
    return uri[0] == '/' || (uri[0] == '*' && uri[1] == '\0');
}

static void process_new_connection(struct mg_connection *conn) {
    struct mg_request_info *ri = &conn->request_info;
    int keep_alive_enabled, keep_alive, discard_len;
    char ebuf[100];

    keep_alive_enabled = !strcmp(conn->ctx->config[op("enable_keep_alive")], "yes");
    keep_alive = 0;

    // Important: on new connection, reset the receiving buffer. Credit goes
    // to crule42.
    conn->data_len = 0;
    do {
        if (!getreq(conn, ebuf, sizeof(ebuf))) {
            response_error(conn, 500, "Server Error", "%s", ebuf);
            conn->must_close = 1;
        } else if (!is_valid_uri(conn->request_info.uri)) {
            snprintf(ebuf, sizeof(ebuf), "Invalid URI: [%s]", ri->uri);
            response_error(conn, 400, "Bad Request", "%s", ebuf);
        } else if (strcmp(ri->http_version, "1.0") &&
                   strcmp(ri->http_version, "1.1")) {
            snprintf(ebuf, sizeof(ebuf), "Bad HTTP version: [%s]", ri->http_version);
            response_error(conn, 505, "Bad HTTP version", "%s", ebuf);
        }

        if (ebuf[0] == '\0') {
            dispatch_and_send_response(conn);
            call_user(MG_REQUEST_END, conn, (void *) (long) conn->status_code);
            log_access(conn);
        }
        if (ri->remote_user != NULL) {
            free((void *) ri->remote_user);
            // Important! When having connections with and without auth
            // would cause double free and then crash
            ri->remote_user = NULL;
        }

        // NOTE(lsm): order is important here. should_keep_alive() call
        // is using parsed request, which will be invalid after memmove's below.
        // Therefore, memorize should_keep_alive() result now for later use
        // in loop exit condition.
        keep_alive = conn->ctx->stop_flag == 0 && keep_alive_enabled &&
            conn->content_len >= 0 && should_keep_alive(conn);

        // Discard all buffered data for this request
        discard_len = conn->content_len >= 0 && conn->request_len > 0 &&
            conn->request_len + conn->content_len < (int64_t) conn->data_len ?
            (int) (conn->request_len + conn->content_len) : conn->data_len;
        assert(discard_len >= 0);
        memmove(conn->buf, conn->buf + discard_len, conn->data_len - discard_len);
        conn->data_len -= discard_len;
        assert(conn->data_len >= 0);
        assert(conn->data_len <= conn->buf_size);
    } while (keep_alive);
}

// Worker threads take accepted socket from the queue
static int consume_socket(struct mg_context *ctx, struct socket *sp) {
    (void) pthread_mutex_lock(&ctx->mutex);
    DEBUG_TRACE(("going idle"));

    // If the queue is empty, wait. We're idle at this point.
    while (ctx->sq_head == ctx->sq_tail && ctx->stop_flag == 0) {
        pthread_cond_wait(&ctx->sq_full, &ctx->mutex);
    }

    // If we're stopping, sq_head may be equal to sq_tail.
    if (ctx->sq_head > ctx->sq_tail) {
        // Copy socket from the queue and increment tail
        *sp = ctx->queue[ctx->sq_tail % ARRAY_SIZE(ctx->queue)];
        ctx->sq_tail++;
        DEBUG_TRACE(("grabbed socket %d, going busy", sp->sock));

        // Wrap pointers if needed
        while (ctx->sq_tail > (int) ARRAY_SIZE(ctx->queue)) {
            ctx->sq_tail -= ARRAY_SIZE(ctx->queue);
            ctx->sq_head -= ARRAY_SIZE(ctx->queue);
        }
    }

    (void) pthread_cond_signal(&ctx->sq_empty);
    (void) pthread_mutex_unlock(&ctx->mutex);

    return !ctx->stop_flag;
}

static void *callback_worker_thread(void *thread_func_param) {
    struct mg_context *ctx = (struct mg_context *) thread_func_param;
    struct mg_connection *conn;

    conn = (struct mg_connection *) calloc(1, sizeof(*conn) + MAX_REQUEST_SIZE);
    if (conn == NULL) {
        cry(create_fake_connection(ctx), "%s", "Cannot create new connection struct, OOM");
    } else {
        conn->buf_size = MAX_REQUEST_SIZE;
        conn->buf = (char *) (conn + 1);
        conn->ctx = ctx;
        conn->event.user_data = ctx->user_data;

        call_user(MG_THREAD_BEGIN, conn, NULL);

        // Call consume_socket() even when ctx->stop_flag > 0, to let it signal
        // sq_empty condvar to wake up the master waiting in produce_socket()
        while (consume_socket(ctx, &conn->client)) {
            conn->birth_time = time(NULL);

            // Fill in IP, port info early so even if SSL setup below fails,
            // error handler would have the corresponding info.
            // Thanks to Johannes Winkelmann for the patch.
            // TODO(lsm): Fix IPv6 case
            conn->request_info.remote_port = ntohs(conn->client.rsa.sin.sin_port);
            memcpy(&conn->request_info.remote_ip,
                   &conn->client.rsa.sin.sin_addr.s_addr, 4);
            conn->request_info.remote_ip = ntohl(conn->request_info.remote_ip);
            conn->request_info.is_ssl = 0;

            process_new_connection(conn);

            close_connection(conn);
        }
        call_user(MG_THREAD_END, conn, NULL);
        free(conn);
    }

    // Signal master that we're done with connection and exiting
    (void) pthread_mutex_lock(&ctx->mutex);
    ctx->num_threads--;
    (void) pthread_cond_signal(&ctx->cond);
    assert(ctx->num_threads >= 0);
    (void) pthread_mutex_unlock(&ctx->mutex);

    DEBUG_TRACE(("exiting"));
    return NULL;
}

// Master thread adds accepted socket to a queue
static void produce_socket(struct mg_context *ctx, const struct socket *sp) {
    (void) pthread_mutex_lock(&ctx->mutex);

    // If the queue is full, wait
    while (ctx->stop_flag == 0 &&
           ctx->sq_head - ctx->sq_tail >= (int) ARRAY_SIZE(ctx->queue)) {
        (void) pthread_cond_wait(&ctx->sq_empty, &ctx->mutex);
    }

    if (ctx->sq_head - ctx->sq_tail < (int) ARRAY_SIZE(ctx->queue)) {
        // Copy socket to the queue and increment head
        ctx->queue[ctx->sq_head % ARRAY_SIZE(ctx->queue)] = *sp;
        ctx->sq_head++;
        DEBUG_TRACE(("queued socket %d", sp->sock));
    }

    (void) pthread_cond_signal(&ctx->sq_full);
    (void) pthread_mutex_unlock(&ctx->mutex);
}

static int set_sock_timeout(SOCKET sock, int milliseconds) {
    struct timeval t;
    t.tv_sec = milliseconds / 1000;
    t.tv_usec = (milliseconds * 1000) % 1000000;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *) &t, sizeof(t)) ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (void *) &t, sizeof(t));
}

static void accept_new_connection(const SOCKET sock,
                                  struct mg_context *ctx) {
    struct socket so;
    socklen_t len = sizeof(so.rsa);
    int on = 1;

    so.sock = accept(sock, &so.rsa.sa, &len);
    if (so.sock == INVALID_SOCKET) {
    } else if (0) {
    } else {
        // Put so socket structure into the queue
        DEBUG_TRACE(("Accepted socket %d", (int) so.sock));
        set_close_on_exec(so.sock);
        getsockname(so.sock, &so.lsa.sa, &len);
        // Set TCP keep-alive. This is needed because if HTTP-level keep-alive
        // is enabled, and client resets the connection, server won't get
        // TCP FIN or RST and will keep the connection open forever. With TCP
        // keep-alive, next keep-alive handshake will figure out that the client
        // is down and will close the server end.
        // Thanks to Igor Klopov who suggested the patch.
        setsockopt(so.sock, SOL_SOCKET, SO_KEEPALIVE, (void *) &on, sizeof(on));
        set_sock_timeout(so.sock, atoi(ctx->config[op("request_timeout_ms")]));
        produce_socket(ctx, &so);
    }
}

static void *callback_master_thread(void *thread_func_param) {
    struct mg_context *ctx = (struct mg_context *) thread_func_param;
    struct pollfd *pfd;

#if defined(ISSUE_317)
    struct sched_param sched_param;
    sched_param.sched_priority = sched_get_priority_max(SCHED_RR);
    pthread_setschedparam(pthread_self(), SCHED_RR, &sched_param);
#endif

    call_user(MG_THREAD_BEGIN, create_fake_connection(ctx), NULL);

    pfd = (struct pollfd *) calloc(1, sizeof(pfd[0]));
    while (pfd != NULL && ctx->stop_flag == 0) {
            pfd[0].fd = ctx->listening_socket_fd;
            pfd[0].events = POLLIN;

        if (poll(pfd, 1, 200) > 0) {
                // NOTE(lsm): on QNX, poll() returns POLLRDNORM after the
                // successfull poll, and POLLIN is defined as (POLLRDNORM | POLLRDBAND)
                // Therefore, we're checking pfd[i].revents & POLLIN, not
                // pfd[i].revents == POLLIN.
                if (ctx->stop_flag == 0 && (pfd[0].revents & POLLIN)) {
                    accept_new_connection(ctx->listening_socket_fd, ctx);
                }
        }
    }
    free(pfd);
    DEBUG_TRACE(("stopping workers"));

    // Stop signal received: somebody called mg_stop. Quit.
    close_all_listening_sockets(ctx);

    // Wakeup workers that are waiting for connections to handle.
    pthread_cond_broadcast(&ctx->sq_full);

    // Wait until all threads finish
    (void) pthread_mutex_lock(&ctx->mutex);
    while (ctx->num_threads > 0) {
        (void) pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    (void) pthread_mutex_unlock(&ctx->mutex);

    // All threads exited, no sync is needed. Destroy mutex and condvars
    (void) pthread_mutex_destroy(&ctx->mutex);
    (void) pthread_cond_destroy(&ctx->cond);
    (void) pthread_cond_destroy(&ctx->sq_empty);
    (void) pthread_cond_destroy(&ctx->sq_full);

    DEBUG_TRACE(("exiting"));

    call_user(MG_THREAD_END, create_fake_connection(ctx), NULL);

    // Signal mg_stop() that we're done.
    // WARNING: This must be the very last thing this
    // thread does, as ctx becomes invalid after this line.
    ctx->stop_flag = 2;
    return NULL;
}

void free_context(struct mg_context *ctx) {
    int i;

    // Deallocate config parameters
    for (i = 0; i < NUM_OPTIONS; i++) {
        if (ctx->config[i] != NULL) {
            // this free causes "double free corruption", i don't know why :(
            //free(ctx->config[i]);
        }
    }

    // Deallocate context itself
    free(ctx);
}

void mg_stop(struct mg_context *ctx) {
    ctx->stop_flag = 1;

    // Wait until mg_fini() stops
    while (ctx->stop_flag != 2) {
        (void) mg_sleep(10);
    }
    free_context(ctx);
}

//-- end of src/mingoose.c --

// src/main.c


static void signal_handler(int sig_num) {
    // Reinstantiate signal handler
    signal(sig_num, signal_handler);


    // Do not do the trick with ignoring SIGCHLD, cause not all OSes (e.g. QNX)
    // reap zombies if SIGCHLD is ignored. On QNX, for example, waitpid()
    // fails if SIGCHLD is ignored, making system() non-functional.
    if (sig_num == SIGCHLD) {
        do {} while (waitpid(-1, &sig_num, WNOHANG) > 0);
    } else { exit_flag = sig_num; }
}

void die(const char *fmt, ...) {
    va_list ap;
    char msg[200];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

void show_usage_and_exit(void) {
    int i;

    fprintf(stderr, "Mingoose version %s (c) DQNEO, built on %s\n",
            mg_version(), __DATE__);
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  mingoose [-option value ...]\n");
    fprintf(stderr, "\nOPTIONS:\n");

    for (i = 0; config_options[i] != NULL; i++) {
        fprintf(stderr, "  -%s\n",
                config_options[i]);
    }
    exit(EXIT_FAILURE);
}


static int event_handler(struct mg_event *event) {
    if (event->type == MG_EVENT_LOG) {
        printf("%s\n", (const char *) event->event_param);
    }
    return 0;
}

// Start Mongoose
int main(int argc, char *argv[]) {

    int i;
    // Show usage if -h or --help options are specified
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        show_usage_and_exit();
    }

    // Setup signal handler: quit on Ctrl-C
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGCHLD, signal_handler);

    // Allocate context and initialize reasonable general case defaults.
    // TODO(lsm): do proper error handling here.
    if ((ctx = (struct mg_context *) calloc(1, sizeof(*ctx))) == NULL) {
        die("%s", "Failed to start Mongoose.");
    }
    ctx->event_handler = event_handler;
    ctx->user_data = NULL;

    set_options(ctx, argv);

    // NOTE(lsm): order is important here. SSL certificates must
    // be initialized before listening ports. UID must be set last.
    if (!check_globalpassfile(ctx) ||
        !socket_bind_listen(ctx) ||
        !mg_setuid(ctx)) {
        free_context(ctx);
        die("%s", "Failed to start Mongoose.");
    }

    // Ignore SIGPIPE signal, so if browser cancels the request, it
    // won't kill the whole process.
    (void) signal(SIGPIPE, SIG_IGN);

    (void) pthread_mutex_init(&ctx->mutex, NULL);
    (void) pthread_cond_init(&ctx->cond, NULL);
    (void) pthread_cond_init(&ctx->sq_empty, NULL);
    (void) pthread_cond_init(&ctx->sq_full, NULL);

    // Start master (listening) thread
    mg_start_thread(callback_master_thread, ctx);

    // Start worker threads
    for (i = 0; i < ctx->settings.num_threads; i++) {
        if (mg_start_thread(callback_worker_thread, ctx) != 0) {
            cry(create_fake_connection(ctx), "Cannot start worker thread: %ld", (long) ERRNO);
        } else {
            ctx->num_threads++;
        }
    }

    printf("Mingoose v.%s started on port(s) %s with web root [%s]\n"
           ,mg_version()
           ,ctx->settings.ports
           ,ctx->settings.document_root
        );

    //enter into endless loop
    while (exit_flag == 0) {
        sleep(1);
    }
    printf("Exiting on signal[%d], waiting for all threads to finish...",
           exit_flag);
    fflush(stdout);
    mg_stop(ctx);
    printf("%s", " done.\n");

    return EXIT_SUCCESS;
}

