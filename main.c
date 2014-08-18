#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <alloca.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "http_download.h"

int http_dl_log_level = 7;

static char *http_dl_agent_string = "Mozilla/5.0 (Windows NT 6.1; WOW64) " \
                                    "AppleWebKit/537.36 (KHTML, like Gecko) " \
                                    "Chrome/35.0.1916.153 Safari/537.36";
static char *http_dl_agent_string_genuine = "Wget/1.5.3";
static http_dl_list_t http_dl_list_initial;
static http_dl_list_t http_dl_list_downloading;
static http_dl_list_t http_dl_list_finished;

/* Count the digits in a (long) integer.  */
static int http_dl_numdigit(long a)
{
    int res = 1;

    while ((a /= 10) != 0) {
        ++res;
    }

    return res;
}

static int http_dl_conn(char *hostname, unsigned short port)
{
    int ret;
    struct sockaddr_in sa;

    if (hostname == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    bzero(&sa, sizeof(sa));
    ret = inet_pton(AF_INET, hostname, &sa.sin_addr);
    if (ret != 1) {
        return -HTTP_DL_ERR_INVALID;
    }

    /* Set port and protocol */
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    /* Make an internet socket, stream type.  */
    if ((ret = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        return -HTTP_DL_ERR_SOCK;
    }

    /* Connect the socket to the remote host.  */
    if (connect(ret, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(ret);
        return -HTTP_DL_ERR_CONN;
    }

    http_dl_log_debug("Created and connected socket fd %d.", ret);

    return ret;
}

static int http_dl_iwrite(int fd, char *buf, int len)
{
    int res = 0;

    if (buf == NULL || len <= 0) {
        return -HTTP_DL_ERR_INVALID;
    }

    /* 'write' may write less than LEN bytes, thus the outward loop
    keeps trying it until all was written, or an error occurred.  The
    inner loop is reserved for the usual EINTR f*kage, and the
    innermost loop deals with the same during select().  */
    while (len > 0) {
        do {
            res = write(fd, buf, len);
        } while (res == -1 && errno == EINTR);
        if (res <= 0) {
            return -HTTP_DL_ERR_WRITE;
        }
        buf += res;
        len -= res;
    }
    return len;
}

static void *http_dl_xrealloc(void *obj, size_t size)
{
    void *res;

    /* Not all Un*xes have the feature of realloc() that calling it with
    a NULL-pointer is the same as malloc(), but it is easy to
    simulate.  */
    if (obj) {
        res = realloc(obj, size);
    } else {
        res = malloc(size);
    }

    if (res == NULL) {
        http_dl_log_debug("allocate %d failed", size);
    }

    return res;
}

#if 0
/* Parse the `Content-Range' header and extract the information it
   contains.  Returns 1 if successful, 0 otherwise.  */
static int http_dl_header_parse_range(const char *hdr, void *arg)
{
    http_dl_range_t *closure = (http_dl_range_t *)arg;
    long num;

    /* Certain versions of Nutscape proxy server send out
    `Content-Length' without "bytes" specifier, which is a breach of
    RFC2068 (as well as the HTTP/1.1 draft which was current at the
    time).  But hell, I must support it...  */
    if (strncasecmp(hdr, "bytes", 5) == 0) {
        hdr += 5;
        hdr += http_dl_clac_lws(hdr);
        if (!*hdr) {
            return 0;
        }
    }

    if (!isdigit(*hdr)) {
        return 0;
    }

    for (num = 0; isdigit(*hdr); hdr++) {
        num = 10 * num + (*hdr - '0');
    }

    if (*hdr != '-' || !isdigit(*(hdr + 1))) {
        return 0;
    }

    closure->first_byte_pos = num;
    ++hdr;

    for (num = 0; isdigit(*hdr); hdr++) {
        num = 10 * num + (*hdr - '0');
    }

    if (*hdr != '/' || !isdigit(*(hdr + 1))) {
        return 0;
    }

    closure->last_byte_pos = num;
    ++hdr;

    for (num = 0; isdigit(*hdr); hdr++) {
        num = 10 * num + (*hdr - '0');
    }

    closure->entity_length = num;
    return 1;
}
#endif

static void http_dl_reset_time(http_dl_info_t *di)
{
    if (di == NULL) {
        return;
    }

    gettimeofday(&di->start_time, NULL);
    di->elapsed_time = -1;  /* for unsigned long, -1 means maximum time */
}

/* unit is msecs */
static unsigned long http_dl_calc_elapsed(http_dl_info_t *di)
{
    struct timeval t;
    unsigned long ret;

    if (di == NULL) {
        return -1;
    }

    gettimeofday(&t, NULL);
    ret = (t.tv_sec - di->start_time.tv_sec) * 1000
           + (t.tv_usec - di->start_time.tv_usec) / 1000;
    if (ret == 0) {
        ret = 100;  /* ������㲻��delta time����ôǿ����Ϊ100ms����������ٶ� */
    }
    di->elapsed_time = ret;

    return ret;
}

static http_dl_info_t *http_dl_create_info(char *url)
{
    http_dl_info_t *di;
    int url_len;
    char *p, *host, *path, *local;
    int host_len, path_len, local_len;
    int port = 0;

    if (url == NULL) {
        http_dl_log_debug("invalid input url");
        return NULL;
    }

    url_len = strlen(url);
    if (url_len >= HTTP_DL_URL_LEN) {
        http_dl_log_debug("url is longer than %d: %s", HTTP_DL_URL_LEN - 1, url);
        return NULL;
    }

    if ((p = strstr(url, HTTP_URL_PREFIX)) != NULL) {
        p = p + HTTP_URL_PRE_LEN;
    } else {
        p = url;
    }

    /* ����host���Լ����ܴ��ڵ�port */
    for (host = p, host_len = 0; *p != '/' && *p != ':' && *p != '\0'; p++, host_len++) {
        (void)0;
    }
    if (*p == ':') {
        p++;
        while (isdigit(*p)) {
            port = port * 10 + (*p - '0');
            if (port > 0xFFFF) {
                http_dl_log_debug("invalid port: %s", url);
                return NULL;
            }
            p++;
        }
        if (*p != '/') {
            http_dl_log_debug("invalid port: %s", url);
            return NULL;
        }
    } else if (*p == '\0') {
        http_dl_log_debug("invalid host: %s", host);
        return NULL;
    }
    if (host_len <= 0 || host_len >= HTTP_DL_HOST_LEN) {
        http_dl_log_debug("invalid host length: %s", host);
        return NULL;
    }

    /* ����path */
    for (path = p, path_len = 0; *p != '\0' && *p != ' ' && *p != '\n'; p++, path_len++) {
        (void)0;
    }
    if (path_len <= 0 || path_len >= HTTP_DL_PATH_LEN) {
        http_dl_log_debug("invalid path length: %s", path);
        return NULL;
    }

    /* �������ر����ļ���local */
    p--;
    for (local_len = 0; *p != '/'; p--, local_len++) {
        (void)0;
    }
    local = p + 1;
    if (local_len <= 0 || local_len >= HTTP_DL_LOCAL_LEN) {
        http_dl_log_debug("invalid local file name: %s", local);
        return NULL;
    }

    di = http_dl_xrealloc(NULL, sizeof(http_dl_info_t));
    if (di == NULL) {
        http_dl_log_debug("allocate failed");
        return NULL;
    }

    bzero(di, sizeof(http_dl_info_t));
    memcpy(di->url, url, url_len);
    memcpy(di->host, host, host_len);
    memcpy(di->path, path, path_len);
    memcpy(di->local, local, local_len);
    if (port != 0) {
        di->port = port;
    } else {
        di->port = 80;  /* Ĭ�ϵ�http����˿� */
    }

    di->stage = HTTP_DL_STAGE_INIT;
    INIT_LIST_HEAD(&di->list);

    di->recv_len = 0;
    di->content_len = 0;
    di->restart_len = 0;
    di->status_code = HTTP_DL_OK;
    di->sockfd = -1;
    di->filefd = -1;

    di->buf_data = di->buf;
    di->buf_tail = di->buf;

    return di;
}

static void http_dl_add_info_to_list(http_dl_info_t *info, http_dl_list_t *list)
{
    if (info == NULL || list == NULL) {
        return;
    }

    list_add_tail(&info->list, &list->list);
    list->count++;
}

static void http_dl_add_info_to_download_list(http_dl_info_t *info)
{
    if (info == NULL) {
        return;
    }

    http_dl_add_info_to_list(info, &http_dl_list_downloading);
    if (http_dl_list_downloading.maxfd < info->sockfd) {
        http_dl_list_downloading.maxfd = info->sockfd;
    }
}

static void http_dl_del_info_from_download_list(http_dl_info_t *info)
{
    if (info == NULL) {
        return;
    }

    list_del_init(&info->list);
    http_dl_list_downloading.count--;
    if (http_dl_list_downloading.count == 0) {
        http_dl_list_downloading.maxfd = -1;
    } else {
        /*
         * ע��: ��ʱӦ����downloading list���ҳ��µ�maxfd����ʹ������ķ������ܿ����ҵ�һ��
         *       ����ô��ȷ�������õ�ֵ
         */
        if (http_dl_list_downloading.maxfd == info->sockfd) {
            http_dl_list_downloading.maxfd = info->sockfd - 1;
        }
    }
}

static void http_dl_init()
{
    http_dl_list_initial.count = 0;
    http_dl_list_initial.maxfd = -1;
    INIT_LIST_HEAD(&http_dl_list_initial.list);
    sprintf(http_dl_list_initial.name, "Initial list");

    http_dl_list_downloading.count = 0;
    http_dl_list_downloading.maxfd = -1;
    INIT_LIST_HEAD(&http_dl_list_downloading.list);
    sprintf(http_dl_list_downloading.name, "Downloading list");

    http_dl_list_finished.count = 0;
    http_dl_list_finished.maxfd = -1;
    INIT_LIST_HEAD(&http_dl_list_finished.list);
    sprintf(http_dl_list_finished.name, "Finished list");
}

static void http_dl_list_destroy(http_dl_list_t *list)
{
    http_dl_info_t *info, *next_info;

    if (list == NULL) {
        return;
    }

    list_for_each_entry_safe(info, next_info, &list->list, list, http_dl_info_t) {
        http_dl_log_debug("[%s] delete %s", list->name, info->url);
        list_del_init(&info->list);
        http_dl_free(info);
        list->count--;
    }

    if (list->count != 0) {
        http_dl_log_error("[%s] FATAL error, after destroy list->count %d (should 0).",
                                list->name, list->count);
    } else {
        http_dl_log_debug("[%s] destroy success.", list->name);
    }
}

static void http_dl_destroy()
{
    http_dl_list_destroy(&http_dl_list_initial);
    http_dl_list_destroy(&http_dl_list_downloading);
    http_dl_list_destroy(&http_dl_list_finished);
}

static void http_dl_list_debug(http_dl_list_t *list)
{
    http_dl_info_t *info;

    if (list == NULL) {
        return;
    }

    http_dl_print_raw("\n%s [%d]:\n", list->name, list->count);
    list_for_each_entry(info, &list->list, list, http_dl_info_t) {
        if (info->recv_len == 0) {
            http_dl_print_raw("\t%s\n", info->url);
        } else if (info->elapsed_time == -1) {
            http_dl_print_raw("\t%s [%ld B/%ld B]\n",
                                info->local, info->recv_len, info->content_len);
        } else {
            http_dl_print_raw("\t%s [%ld B/%ld B] [%ld KB/s]\n",
                                info->local,
                                info->recv_len,
                                info->content_len,
                                info->recv_len / info->elapsed_time);
        }
    }
    http_dl_print_raw("--------------\n");
}

static void http_dl_debug_show()
{
    http_dl_list_debug(&http_dl_list_initial);
    http_dl_list_debug(&http_dl_list_downloading);
    http_dl_list_debug(&http_dl_list_finished);
}

static int http_dl_send_req(http_dl_info_t *di)
{
    int ret, nwrite;
    char range[HTTP_DL_BUF_LEN], *useragent;
    char *request;
    int request_len;
    char *command = "GET";

    if (di == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    if (di->stage < HTTP_DL_STAGE_SEND_REQUEST) {
        ret = http_dl_conn(di->host, di->port);
        if (ret < 0) {
            http_dl_log_debug("connect failed: %s:%d", di->host, di->port);
            return -HTTP_DL_ERR_CONN;
        }
        di->sockfd = ret;
        di->stage = HTTP_DL_STAGE_SEND_REQUEST;
    }

    bzero(range, sizeof(range));
    if (di->restart_len != 0) { /* �ϵ����� */
        if (sizeof(range) < (http_dl_numdigit(di->restart_len) + 17)) {
            http_dl_log_error("range string is longer than %d", sizeof(range) - 17);
            return -HTTP_DL_ERR_INVALID;
        }
        sprintf(range, "Range: bytes=%ld-\r\n", di->restart_len);
    }

    if (di->flags & HTTP_DL_F_GENUINE_AGENT) {
        useragent = http_dl_agent_string_genuine;
    } else {
        useragent = http_dl_agent_string;
    }

    request_len = strlen(command) + strlen(di->path)
                + strlen(useragent)
                + strlen(di->host) + http_dl_numdigit(di->port)
                + strlen(HTTP_ACCEPT)
                + strlen(range)
                + 64;
    request = http_dl_xrealloc(NULL, request_len);
    if (request == NULL) {
        http_dl_log_error("allocate request buffer %d failed.", request_len);
        return -HTTP_DL_ERR_RESOURCE;
    }

    bzero(request, request_len);
    sprintf(request, "%s %s HTTP/1.0\r\n"
                     "User-Agent: %s\r\n"
                     "Host: %s:%d\r\n"
                     "Accept: %s\r\n"
                     "%s\r\n",
                     command, di->path,
                     useragent,
                     di->host, di->port,
                     HTTP_ACCEPT,
                     range);
    http_dl_log_debug("\n--- request begin ---\n%s--- request end ---\n", request);

    nwrite = http_dl_iwrite(di->sockfd, request, strlen(request));
    if (nwrite < 0) {
        http_dl_log_debug("write HTTP request failed.");
        ret = -HTTP_DL_ERR_WRITE;
        goto err_out;
    }

    http_dl_log_info("HTTP request sent, awaiting response...");
    ret = HTTP_DL_OK;

err_out:
    http_dl_free(request);

    return ret;
}

static void http_dl_list_proc_initial()
{
    http_dl_list_t *dl_list;
    http_dl_info_t *info, *next_info;
    int res;

    dl_list = &http_dl_list_initial;

    if (dl_list->count == 0) {
        return;
    }

    list_for_each_entry_safe(info, next_info, &dl_list->list, list, http_dl_info_t) {
        list_del_init(&info->list);
        dl_list->count--;
        res = http_dl_send_req(info);
        if (res == HTTP_DL_OK) {
            http_dl_add_info_to_download_list(info);
        } else {
            http_dl_log_debug("re-add %s to %s", info->url, dl_list->name);
            http_dl_add_info_to_list(info, dl_list);
        }
    }

    return;
}

static int http_dl_parse_status_line(http_dl_info_t *info)
{
    int reason_nbytes;
    int mjr, mnr, statcode;
    char *line_end, *p;

    if (info == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    if (info->stage != HTTP_DL_STAGE_PARSE_STATUS_LINE) {
        http_dl_log_debug("Wrong stage %d.", info->stage);
        return -HTTP_DL_ERR_INTERNAL;
    }

    *(info->buf_tail) = '\0';   /* �ַ�������ʱ��ȷ����Խ�� */

    line_end = strstr(info->buf_data, "\r\n");
    if (line_end == NULL) {
        /* status line��û��������������... */
        http_dl_log_debug("Incompleted status line: %s", info->buf_data);
        return HTTP_DL_OK;
    }

    /* The standard format of HTTP-Version is: `HTTP/X.Y', where X is
     major version, and Y is minor version.  */
    if (strncmp(info->buf_data, "HTTP/", 5) != 0) {
        http_dl_log_debug("Invalid status line: %s", info->buf_data);
        return -HTTP_DL_ERR_INVALID;
    }
    info->buf_data += 5;

    /* Calculate major HTTP version.  */
    p = info->buf_data;
    for (mjr = 0; isdigit(*p); p++) {
        mjr = 10 * mjr + (*p - '0');
    }
    if (*p != '.' || p == info->buf_data) {
        http_dl_log_debug("Invalid status line: %s", info->buf_data);
        return -HTTP_DL_ERR_INVALID;
    }
    p++;
    info->buf_data = p;

    /* Calculate minor HTTP version.  */
    for (mnr = 0; isdigit(*p); p++) {
        mnr = 10 * mnr + (*p - '0');
    }
    if (*p != ' ' || p == info->buf_data) {
        http_dl_log_debug("Invalid status line: %s", info->buf_data);
        return -HTTP_DL_ERR_INVALID;
    }

    http_dl_log_debug("Version is HTTP/%d.%d", mjr, mnr);

    p++;
    info->buf_data = p;

    /* Calculate status code.  */
    if (!(isdigit(p[0]) && isdigit(p[1]) && isdigit(p[2]))) {
        http_dl_log_debug("Invalid status line: %s", info->buf_data);
        return -HTTP_DL_ERR_INVALID;
    }
    statcode = 100 * (p[0] - '0') + 10 * (p[1] - '0') + (p[2] - '0');
    http_dl_log_debug("Status code is %d", statcode);

    /* Set up the reason phrase pointer.  */
    p += 3;
    info->buf_data = p;
    if (*p != ' ') {
        http_dl_log_debug("Invalid status line: %s", info->buf_data);
        return -HTTP_DL_ERR_INVALID;
    }
    info->buf_data++;
    reason_nbytes = line_end - info->buf_data;
    bzero(info->err_msg, sizeof(info->err_msg));
    memcpy(info->err_msg, info->buf_data, MINVAL((sizeof(info->err_msg) - 1), reason_nbytes));
    info->status_code = statcode;

    http_dl_log_debug("Finish parse HTTP status line: %s", info->err_msg);

    info->stage = HTTP_DL_STAGE_PARSE_HEADER;

    info->buf_data = line_end + 2;  /* �Թ�"\r\n" */
    if (info->buf_data < info->buf_tail) {
        /* ��������δ�����꣬ת����һ��stage���������� */
        return -HTTP_DL_ERR_AGAIN;
    }

    return HTTP_DL_OK;
}

/* Skip LWS (linear white space), if present.  Returns number of
   characters to skip.  */
static int http_dl_clac_lws(const char *string)
{
    const char *p = string;

    if (string == NULL) {
        return 0;
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }

    return (p - string);
}

static int http_dl_header_extract_long_num(const char *val, void *closure)
{
    const char *p = val;
    long result;

    if (val == NULL || closure == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    for (result = 0; isdigit(*p); p++) {
        result = 10 * result + (*p - '0');
    }
    if (*p != '\r') {
        return -HTTP_DL_ERR_INVALID;
    }

    *(long *)closure = result;

    return HTTP_DL_OK;
}

static int http_dl_header_dup_str_to_buf(const char *val, void *buf)
{
    int len;
    char *val_end;

    if (val == NULL || buf == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    val_end = strstr(val, "\r\n");
    if (val_end == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    len = val_end - val;
    if (len <= 0) {
        return -HTTP_DL_ERR_INVALID;
    }

    bzero(buf, HTTP_DL_BUF_LEN);
    memcpy(buf, val, MINVAL(len, HTTP_DL_BUF_LEN - 1));

    return HTTP_DL_OK;
}

#if 0
/* Strdup HEADER, and place the pointer to CLOSURE. XXX �ǵ��ͷŶѿռ�buffer */
static int http_dl_header_alloc_and_dup_str(const char *val, void *closure)
{
    int len;
    char *p, *val_end;

    if (val == NULL || closure == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    val_end = strstr(val, "\r\n");
    if (val_end == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    len = val_end - val;
    if (len <= 0) {
        return -HTTP_DL_ERR_INVALID;
    }
    len++;

    p = http_dl_xrealloc(NULL, len);
    if (p == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }
    bzero(p, len);
    memcpy(p, val, len);

    *(char **)closure = p;

    return HTTP_DL_OK;
}

/* �����"none"����ô��*closure��1��������0 */
static int http_dl_header_judge_str_none(const char *val, void *closure)
{
    char *val_end, *res;
    int *where = (int *)closure;

    if (val == NULL || closure == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    if ((val_end = strstr(val, "\r\n")) == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    res = strstr(val, "none");
    if (res != NULL && res < val_end) {
        *where = 1;
    } else {
        *where = 0;
    }

    return HTTP_DL_OK;
}
#endif

/*
 * ����-HTTP_DL_ERR_INVALID��ʾ��������Ӧֱ��������һ��;
 * ����-HTTP_DL_ERR_NOTFOUND��ʾname�뵱ǰ�в�ƥ�䣬������һ�в���;
 * ����HTTP_DL_OK����ʾ��ǰ�����������ɴ�����һ���ˡ�
 */
static int http_dl_header_process(const char *header,
                                  const char *name,
                                  int (*procfun)(const char *, void *),
                                  void *arg)
{
    const char *val;
    int gap;

    if (header == NULL
        || name == NULL
        || procfun == NULL
        || arg == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    /* Check whether HEADER matches NAME.  */
    while (*name && (tolower(*name) == tolower(*header))) {
        ++name, ++header;
    }

    if (*name || *header++ != ':') {
        return -HTTP_DL_ERR_NOTFOUND;
    }

    gap = http_dl_clac_lws(header);
    val = header;
    val += gap;

    /* ����HTTP_DL_OK��-HTTP_DL_ERR_INVALID */
    return ((*procfun)(val, arg));
}

static int http_dl_parse_header(http_dl_info_t *info)
{
    int ret;
    char *line_end;
    int hlen;
    char print_buf[HTTP_DL_BUF_LEN];

    if (info == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    if (info->stage != HTTP_DL_STAGE_PARSE_HEADER) {
        http_dl_log_debug("Wrong stage %d.", info->stage);
        return -HTTP_DL_ERR_INTERNAL;
    }

    *(info->buf_tail) = '\0';   /* �ַ�������ʱ��ȷ����Խ�� */

    while (1) {
        bzero(print_buf, HTTP_DL_BUF_LEN);
        line_end = strstr(info->buf_data, "\r\n");
        if (line_end == NULL) {
            /* header��û���յ�������һ�У�����... */
            http_dl_log_debug("Incompleted header line: %s", info->buf_data);
            break;
        }

        if (info->buf_data == line_end) {
            /* header����������޸�stageΪRECV_CONTENT������ERR_AGAIN�������½׶δ��� */
            http_dl_reset_time(info);
            info->stage = HTTP_DL_STAGE_RECV_CONTENT;
            info->buf_data += 2;
            return -HTTP_DL_ERR_AGAIN;
        }

        ret = http_dl_header_process(info->buf_data,
                                     "Content-Length",
                                     http_dl_header_extract_long_num,
                                     &info->content_len);
        if (ret == HTTP_DL_OK || ret == -HTTP_DL_ERR_INVALID) {
            goto header_line_done;
        }

        ret = http_dl_header_process(info->buf_data,
                                     "Content-Type",
                                     http_dl_header_dup_str_to_buf,
                                     print_buf);
        if (ret == HTTP_DL_OK || ret == -HTTP_DL_ERR_INVALID) {
            if (strlen(print_buf) > 0) {
                http_dl_log_debug("Content-Type: %s", print_buf);
            }
            goto header_line_done;
        }

        ret = http_dl_header_process(info->buf_data,
                                     "Accept-Ranges",
                                     http_dl_header_dup_str_to_buf,
                                     print_buf);
        if (ret == HTTP_DL_OK || ret == -HTTP_DL_ERR_INVALID) {
            if (strlen(print_buf) > 0) {
                http_dl_log_debug("Accept-Ranges: %s", print_buf);
            }
            goto header_line_done;
        }

        ret = http_dl_header_process(info->buf_data,
                                     "Content-Range",
                                     http_dl_header_dup_str_to_buf,
                                     print_buf);
        if (ret == HTTP_DL_OK || ret == -HTTP_DL_ERR_INVALID) {
            if (strlen(print_buf) > 0) {
                http_dl_log_debug("Content-Range: %s", print_buf);
            }
            goto header_line_done;
        }

        ret = http_dl_header_process(info->buf_data,
                                     "Last-Modified",
                                     http_dl_header_dup_str_to_buf,
                                     print_buf);
        if (ret == HTTP_DL_OK || ret == -HTTP_DL_ERR_INVALID) {
            if (strlen(print_buf) > 0) {
                http_dl_log_debug("Last-Modified: %s", print_buf);
            }
            goto header_line_done;
        }

        hlen = line_end - info->buf_data;
        if (hlen > 0){
            memcpy(print_buf, info->buf_data, MINVAL(hlen, (HTTP_DL_BUF_LEN - 1)));
            http_dl_log_debug("Unsupported header: %s", print_buf);
        }

header_line_done:
        /* ��Ӧ������ɻ��������� */
        if (ret == -HTTP_DL_ERR_INVALID) {
            http_dl_log_error("Invalid header line: %s", info->buf_data);
        }
        info->buf_data = line_end + 2;
    }

    return HTTP_DL_OK;
}

static inline void http_dl_move_data(char *dst, char *src, char *end)
{
    while (src < end) {
        *dst = *src;
        dst++;
        src++;
    }
}

static void http_dl_adjust_info_buf(http_dl_info_t *info)
{
    int data_len, free_space;

    if (info == NULL) {
        http_dl_log_debug("ERROR argument is NULL.");
        return;
    }

    if (info->buf_data == info->buf_tail) {
        /* info->buf�������Ѿ�������� */
        bzero(info->buf, HTTP_DL_READBUF_LEN);
        info->buf_data = info->buf;
        info->buf_tail = info->buf;
        return;
    }

    data_len = info->buf_tail - info->buf_data;

    free_space = info->buf + HTTP_DL_READBUF_LEN - info->buf_tail;
    if (free_space < (HTTP_DL_READBUF_LEN >> 2)) {
        http_dl_log_debug("buf_tail<%p> reaching buffer end<%p>, adjust buffer...",
                            info->buf_tail, info->buf + HTTP_DL_READBUF_LEN);
        http_dl_move_data(info->buf, info->buf_data, info->buf_tail);
        info->buf_data = info->buf;
        info->buf_tail = info->buf_data + data_len;

        return;
    } else if ((free_space < (HTTP_DL_READBUF_LEN >> 1))
                && (data_len < (HTTP_DL_READBUF_LEN >> 2))) {
        http_dl_log_debug("free space [%d], and data length [%d], adjust buffer...",
                            free_space, data_len);
        http_dl_move_data(info->buf, info->buf_data, info->buf_tail);
        info->buf_data = info->buf;
        info->buf_tail = info->buf_data + data_len;

        return;
    } else {
        http_dl_log_debug("no adjustment, free[%d], data[%d], buf<%p>, tail<%p>",
                            free_space, data_len, info->buf, info->buf_tail);

        return;
    }
}

static int http_dl_recv_content(http_dl_info_t *info)
{
    int data_len;

    if (info == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    if (info->stage != HTTP_DL_STAGE_RECV_CONTENT) {
        http_dl_log_debug("Wrong stage %d.", info->stage);
        return -HTTP_DL_ERR_INTERNAL;
    }

    if (info->buf_data == info->buf_tail) {
        http_dl_log_debug("No data in buffer.");
        return HTTP_DL_OK;
    }

    data_len = info->buf_tail - info->buf_data;
    info->recv_len += data_len;
    /* XXX TODO: ������յ����ݵ��ļ��� */
    info->buf_data += data_len;

    return HTTP_DL_OK;
}

/*
 * ����HTTP��������Ӧ��������
 */
static int http_dl_recv_resp(http_dl_info_t *info)
{
    int ret;
    int nread, free_space;

    if (info == NULL) {
        return -HTTP_DL_ERR_INVALID;
    }

    if (info->stage <= HTTP_DL_STAGE_SEND_REQUEST) {
        /* ������ձ����ݵĳ�ʼ״̬ */
        info->stage = HTTP_DL_STAGE_PARSE_STATUS_LINE;
    }

    free_space = info->buf + HTTP_DL_READBUF_LEN - info->buf_tail;
    if (free_space < (HTTP_DL_READBUF_LEN >> 1)) {
        http_dl_log_info("WARNING: info buffer free space %d too small, (total %d)",
                            free_space, HTTP_DL_READBUF_LEN);
    }

    nread = read(info->sockfd, info->buf_tail, free_space);
    if (nread == 0) {
        /* XXX TODO: ���ؽ�������Ҫ��info buffer���������ȫ��flush���ļ��� */
        return -HTTP_DL_ERR_EOF;
    } else if (nread < 0) {
        http_dl_log_error("read failed, %d", nread);
        return -HTTP_DL_ERR_READ;
    }

    info->buf_tail += nread;

again:
    switch (info->stage) {
    case HTTP_DL_STAGE_PARSE_STATUS_LINE:
        ret = http_dl_parse_status_line(info);
        break;
    case HTTP_DL_STAGE_PARSE_HEADER:
        ret = http_dl_parse_header(info);
        break;
    case HTTP_DL_STAGE_RECV_CONTENT:
        ret = http_dl_recv_content(info);
        break;
    default:
        http_dl_log_error("Incorrect stage %d in here.", info->stage);
        return -HTTP_DL_ERR_INTERNAL;
        break;
    }

    if (ret == -HTTP_DL_ERR_AGAIN) {
        /* buffer�л����¸�stage������δ������ */
        http_dl_log_debug("Continue next stage process.");
        goto again;
    } else if (ret == HTTP_DL_OK) {
        /* �ֽ׶λ�δ�����꣬�ȴ��´ε��������ݣ��������� */
        /* XXX: ����buffer�д˴�δ����������� */
        (void)http_dl_adjust_info_buf(info);
    } else {
        http_dl_log_debug("Process response failed %d.", ret);
        /* XXX TODO: ��Ҫflush buffer�е�����ô? */
    }

    return ret;
}

static void http_dl_finish_req(http_dl_info_t *info)
{
    if (info == NULL) {
        return;
    }

    if (info->filefd >= 0) {
        http_dl_log_debug("close opened file fd %d", info->filefd);
        close(info->filefd);
        info->filefd = -1;
    }

    if (info->sockfd >= 0) {
        http_dl_log_debug("close opened socket fd %d", info->sockfd);
        close(info->sockfd);
        info->sockfd = -1;
    }

    http_dl_calc_elapsed(info);

    info->stage = HTTP_DL_STAGE_FINISH;
    http_dl_add_info_to_list(info, &http_dl_list_finished);
}

static int http_dl_list_proc_downloading()
{
    http_dl_list_t *dl_list;
    http_dl_info_t *info, *next_info;
    struct timeval tv;
    fd_set rset, rset_org;
    int res, read_res;

    dl_list = &http_dl_list_downloading;
    if (dl_list->count == 0) {
        return -HTTP_DL_ERR_INVALID;
    }

    FD_ZERO(&rset_org);
    list_for_each_entry(info, &dl_list->list, list, http_dl_info_t) {
        FD_SET(info->sockfd, &rset_org);
    }

    while (1) {
        if (dl_list->count == 0) {
            http_dl_log_info("All finished...");
            break;
        }

        bzero(&tv, sizeof(tv));
        tv.tv_sec = HTTP_DL_READ_TIMEOUT;
        tv.tv_usec = 0;

        FD_ZERO(&rset);
        memcpy(&rset, &rset_org, sizeof(fd_set));

        res = select(dl_list->maxfd + 1, &rset, NULL, NULL, &tv);
        if (res == 0) {
            /* ��ʱ */
            http_dl_log_debug("select timeout (%d secs)", HTTP_DL_READ_TIMEOUT);
            continue;
        } else if (res == -1 && errno == EINTR) {
            /* ���ж� */
            http_dl_log_debug("select interrupted by signal.");
            continue;
        } else if (res < 0){
            /* ���� */
            http_dl_log_error("select failed, return %d", res);
            break;
        }

        list_for_each_entry_safe(info, next_info, &dl_list->list, list, http_dl_info_t) {
            if (!FD_ISSET(info->sockfd, &rset)) {
                continue;
            }

            read_res = http_dl_recv_resp(info);
            if (read_res == -HTTP_DL_ERR_EOF) {
                /* �ô����ؽ��� */
                FD_CLR(info->sockfd, &rset_org);
                http_dl_del_info_from_download_list(info);
                http_dl_finish_req(info);
                continue;
            } else if (read_res == HTTP_DL_OK) {
                /* �ô����������������ݣ������ٴμ��� */
                continue;
            } else {
                /* ���أ������������� */
                http_dl_log_error("receive data from %s, sockfd %d failed.",
                                        info->url, info->sockfd);
                return -HTTP_DL_ERR_READ;
            }
        }
    }

    return HTTP_DL_OK;
}

static int http_dl_list_proc_finished()
{
    return HTTP_DL_OK;
}

int main(int argc, char *argv[])
{
    FILE *fp;
    char url_buf[HTTP_DL_URL_LEN];
    int url_len, ret = HTTP_DL_OK;
    http_dl_info_t *di;

    if (argc != 2) {
        http_dl_print_raw("Usage: %s <url_list.txt>\n", argv[0]);
        return -HTTP_DL_ERR_INVALID;
    }

    http_dl_init();

    fp = fopen(argv[1], "r");
    if (fp == NULL) {
        http_dl_log_error("Open file %s failed", argv[1]);
        return -HTTP_DL_ERR_FOPEN;
    }

    while (1) {
        bzero(url_buf, sizeof(url_buf));
        if (fgets(url_buf, sizeof(url_buf), fp) == NULL) {
            break;
        }

        url_len = strlen(url_buf);
        if (url_buf[url_len - 1] != '\n') {
            http_dl_log_error("URL in file %s is too long", argv[1]);
            ret = -HTTP_DL_ERR_INVALID;
            goto err_out;
        }
        url_buf[url_len - 1] = '\0';

        di = http_dl_create_info(url_buf);
        if (di == NULL) {
            http_dl_log_info("Create download task %s failed.", url_buf);
            continue;
        }

        http_dl_add_info_to_list(di, &http_dl_list_initial);

        http_dl_log_info("Create download task %s success.", url_buf);
    }

    http_dl_debug_show();

    http_dl_list_proc_initial();

    http_dl_debug_show();

    http_dl_list_proc_downloading();

    http_dl_debug_show();

    http_dl_list_proc_finished();

    http_dl_debug_show();

err_out:
    http_dl_destroy();
    fclose(fp);

    return ret;
}
