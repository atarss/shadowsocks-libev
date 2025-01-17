/*
 * udprelay.c - Setup UDP relay for both client and server
 *
 * Copyright (C) 2013 - 2015, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifndef __MINGW32__
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(HAVE_SYS_IOCTL_H) && defined(HAVE_NET_IF_H) && defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#define SET_INTERFACE
#endif

#ifdef __MINGW32__
#include "win32.h"
#endif

#include <libcork/core.h>
#include <udns.h>

#include "utils.h"
#include "netutils.h"
#include "cache.h"
#include "udprelay.h"

#ifdef UDPRELAY_REMOTE
#define MAX_UDP_CONN_NUM 1024
#else
#define MAX_UDP_CONN_NUM 256
#endif

#ifdef UDPRELAY_REMOTE
#ifdef UDPRELAY_LOCAL
#error "UDPRELAY_REMOTE and UDPRELAY_LOCAL should not be both defined"
#endif
#endif

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#define BUF_SIZE MAX_UDP_PACKET_SIZE

static void server_recv_cb(EV_P_ ev_io *w, int revents);
static void remote_recv_cb(EV_P_ ev_io *w, int revents);
static void remote_timeout_cb(EV_P_ ev_timer *watcher, int revents);
static char *hash_key(const char *header, const int header_len,
                      const struct sockaddr_storage *addr);
#ifdef UDPRELAY_REMOTE
static void query_resolve_cb(struct sockaddr *addr, void *data);
#endif
static void close_and_free_remote(EV_P_ struct remote_ctx *ctx);
static struct remote_ctx * new_remote(int fd, struct server_ctx * server_ctx);

extern int verbose;

static int server_num = 0;
static struct server_ctx *server_ctx_list[MAX_REMOTE_NUM] = { NULL };

#ifndef __MINGW32__
static int setnonblocking(int fd)
{
    int flags;
    if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
        flags = 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
#endif

#ifdef SET_INTERFACE
static int setinterface(int socket_fd, const char * interface_name)
{
    struct ifreq interface;
    memset(&interface, 0, sizeof(interface));
    strncpy(interface.ifr_name, interface_name, IFNAMSIZ);
    int res = setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, &interface,
                         sizeof(struct ifreq));
    return res;
}
#endif

#if defined(UDPRELAY_REMOTE) && defined(SO_BROADCAST)
static int set_broadcast(int socket_fd)
{
    int opt = 1;
    return setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
}
#endif

#ifdef SO_NOSIGPIPE
static int set_nosigpipe(int socket_fd)
{
    int opt = 1;
    return setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
}
#endif

#ifdef UDPRELAY_REDIR

#ifndef IP_TRANSPARENT
#define IP_TRANSPARENT       19
#endif

#ifndef IP_RECVORIGDSTADDR
#define IP_RECVORIGDSTADDR   20
#endif

static int get_dstaddr(struct msghdr *msg, struct sockaddr_storage *dstaddr)
{
    struct cmsghdr *cmsg;

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVORIGDSTADDR) {
            memcpy(dstaddr, CMSG_DATA(cmsg), sizeof(struct sockaddr_in));
            dstaddr->ss_family = AF_INET;
            return 0;

        } else if (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IP_RECVORIGDSTADDR) {
            memcpy(dstaddr, CMSG_DATA(cmsg), sizeof(struct sockaddr_in6));
            dstaddr->ss_family = AF_INET6;
            return 0;
        }
    }

    return 1;
}
#endif

static char *hash_key(const char *header, const int header_len,
                      const struct sockaddr_storage *addr)
{
    char key[384];

    // calculate hash key
    // assert header_len < 256
    memset(key, 0, 384);
    memcpy(key, addr, sizeof(struct sockaddr_storage));
    memcpy(key + sizeof(struct sockaddr_storage), header, header_len);

    return (char *)enc_md5((const uint8_t *)key,
                           sizeof(struct sockaddr_storage) + header_len, NULL);
}

static int parse_udprealy_header(const char * buf, const int buf_len,
                                 char *host, char *port,
                                 struct sockaddr_storage *storage)
{

    const uint8_t atyp = *(uint8_t *)buf;
    int offset = 1;
    // get remote addr and port
    if (atyp == 1) {
        // IP V4
        size_t in_addr_len = sizeof(struct in_addr);
        if (buf_len > in_addr_len) {
            if (storage != NULL) {
                struct sockaddr_in *addr = (struct sockaddr_in *)storage;
                addr->sin_family = AF_INET;
                addr->sin_addr = *(struct in_addr *)(buf + offset);
                addr->sin_port = *(uint16_t *)(buf + offset + in_addr_len);
            }
            if (host != NULL) {
                dns_ntop(AF_INET, (const void *)(buf + offset),
                         host, INET_ADDRSTRLEN);
            }
            offset += in_addr_len;
        }
    } else if (atyp == 3) {
        // Domain name
        uint8_t name_len = *(uint8_t *)(buf + offset);
        if (name_len < buf_len && name_len < 255 && name_len > 0) {
            if (host != NULL) {
                memcpy(host, buf + offset + 1, name_len);
            }
            offset += name_len + 1;
        }
        if (storage != NULL) {
            struct cork_ip ip;
            if (cork_ip_init(&ip, host) != -1) {
                if (ip.version == 4) {
                    struct sockaddr_in *addr = (struct sockaddr_in *)storage;
                    dns_pton(AF_INET, host, &(addr->sin_addr));
                    addr->sin_port = *(uint16_t *)(buf + offset);
                    addr->sin_family = AF_INET;
                } else if (ip.version == 6) {
                    struct sockaddr_in6 *addr = (struct sockaddr_in6 *)storage;
                    dns_pton(AF_INET, host, &(addr->sin6_addr));
                    addr->sin6_port = *(uint16_t *)(buf + offset);
                    addr->sin6_family = AF_INET6;
                }
            }
        }
    } else if (atyp == 4) {
        // IP V6
        size_t in6_addr_len = sizeof(struct in6_addr);
        if (buf_len > in6_addr_len) {
            if (storage != NULL) {
                struct sockaddr_in6 *addr = (struct sockaddr_in6 *)storage;
                addr->sin6_family = AF_INET6;
                addr->sin6_addr = *(struct in6_addr *)(buf + offset);
                addr->sin6_port = *(uint16_t *)(buf + offset + in6_addr_len);
            }
            if (host != NULL) {
                dns_ntop(AF_INET6, (const void *)(buf + offset),
                         host, INET6_ADDRSTRLEN);
            }
            offset += in6_addr_len;
        }
    }

    if (offset == 1) {
        LOGE("[udp] invalid header with addr type %d", atyp);
        return 0;
    }

    if (port != NULL) {
        sprintf(port, "%d", ntohs(*(uint16_t *)(buf + offset)));
    }
    offset += 2;

    return offset;
}

static char *get_addr_str(const struct sockaddr *sa)
{
    static char s[SS_ADDRSTRLEN];
    memset(s, 0, SS_ADDRSTRLEN);
    char addr[INET6_ADDRSTRLEN] = { 0 };
    char port[PORTSTRLEN] = { 0 };
    uint16_t p;

    switch (sa->sa_family) {
    case AF_INET:
        dns_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
                 addr, INET_ADDRSTRLEN);
        p = ntohs(((struct sockaddr_in *)sa)->sin_port);
        sprintf(port, "%d", p);
        break;

    case AF_INET6:
        dns_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
                 addr, INET6_ADDRSTRLEN);
        p = ntohs(((struct sockaddr_in *)sa)->sin_port);
        sprintf(port, "%d", p);
        break;

    default:
        strncpy(s, "Unknown AF", SS_ADDRSTRLEN);
    }

    int addr_len = strlen(addr);
    int port_len = strlen(port);
    memcpy(s, addr, addr_len);
    memcpy(s + addr_len + 1, port, port_len);
    s[addr_len] = ':';

    return s;
}


int create_remote_socket(int ipv6)
{
    int remote_sock;

    if (ipv6) {
        // Try to bind IPv6 first
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = 0;
        remote_sock = socket(AF_INET6, SOCK_DGRAM, 0);
        if (remote_sock == -1) {
            ERROR("[udp] cannot create socket");
            return -1;
        }
        if (bind(remote_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            FATAL("[udp] cannot bind remote");
            return -1;
        }
    } else {
        // Or else bind to IPv4
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        remote_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (remote_sock == -1) {
            ERROR("[udp] cannot create socket");
            return -1;
        }

        if (bind(remote_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            FATAL("[udp] cannot bind remote");
            return -1;
        }
    }
    return remote_sock;
}

int create_server_socket(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp, *ipv4v6bindall;
    int s, server_sock;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;                 /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_DGRAM;              /* We want a UDP socket */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* For wildcard IP address */
    hints.ai_protocol = IPPROTO_UDP;

    s = getaddrinfo(host, port, &hints, &result);
    if (s != 0) {
        LOGE("[udp] getaddrinfo: %s", gai_strerror(s));
        return -1;
    }

    rp = result;

    /*
       On Linux, with net.ipv6.bindv6only = 0 (the default), getaddrinfo(NULL) with
       AI_PASSIVE returns 0.0.0.0 and :: (in this order). AI_PASSIVE was meant to
       return a list of addresses to listen on, but it is impossible to listen on
       0.0.0.0 and :: at the same time, if :: implies dualstack mode.
     */
    if (!host) {
        ipv4v6bindall = result;

        /* Loop over all address infos found until a IPV6 address is found. */
        while (ipv4v6bindall) {
            if (ipv4v6bindall->ai_family == AF_INET6) {
                rp = ipv4v6bindall; /* Take first IPV6 address available */
                break;
            }
            ipv4v6bindall = ipv4v6bindall->ai_next; /* Get next address info, if any */
        }
    }

    for (/*rp = result*/; rp != NULL; rp = rp->ai_next) {
        server_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_sock == -1) {
            continue;
        }

        if (rp->ai_family == AF_INET6) {
            int ipv6only = host ? 1 : 0;
            setsockopt(server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only));
        }

        int opt = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
        set_nosigpipe(server_sock);
#endif

#ifdef UDPRELAY_REDIR
        if (setsockopt(server_sock, SOL_IP, IP_TRANSPARENT, &opt, sizeof(opt))) {
            FATAL("[udp] setsockopt IP_TRANSPARENT");
        }
        if (setsockopt(server_sock, IPPROTO_IP, IP_RECVORIGDSTADDR, &opt, sizeof(opt))) {
            FATAL("[udp] setsockopt IP_RECVORIGDSTADDR");
        }
#endif

        s = bind(server_sock, rp->ai_addr, rp->ai_addrlen);
        if (s == 0) {
            /* We managed to bind successfully! */
            break;
        } else {
            ERROR("[udp] bind");
        }

        close(server_sock);
    }

    if (rp == NULL) {
        LOGE("[udp] cannot bind");
        return -1;
    }

    freeaddrinfo(result);

    return server_sock;
}

struct remote_ctx *new_remote(int fd, struct server_ctx *server_ctx)
{
    struct remote_ctx *ctx = malloc(sizeof(struct remote_ctx));
    memset(ctx, 0, sizeof(struct remote_ctx));
    ctx->fd = fd;
    ctx->server_ctx = server_ctx;
    ev_io_init(&ctx->io, remote_recv_cb, fd, EV_READ);
    ev_timer_init(&ctx->watcher, remote_timeout_cb, server_ctx->timeout,
                  server_ctx->timeout);
    return ctx;
}

struct server_ctx * new_server_ctx(int fd)
{
    struct server_ctx *ctx = malloc(sizeof(struct server_ctx));
    memset(ctx, 0, sizeof(struct server_ctx));
    ctx->fd = fd;
    ev_io_init(&ctx->io, server_recv_cb, fd, EV_READ);
    return ctx;
}

#ifdef UDPRELAY_REMOTE
struct query_ctx *new_query_ctx(const char *buf, const int buf_len)
{
    struct query_ctx *ctx = malloc(sizeof(struct query_ctx));
    memset(ctx, 0, sizeof(struct query_ctx));
    ctx->buf = malloc(buf_len);
    ctx->buf_len = buf_len;
    memcpy(ctx->buf, buf, buf_len);
    return ctx;
}

void close_and_free_query(EV_P_ struct query_ctx *ctx)
{
    if (ctx != NULL) {
        if (ctx->query != NULL) {
            resolv_cancel(ctx->query);
            ctx->query = NULL;
        }
        if (ctx->buf != NULL) {
            free(ctx->buf);
        }
        free(ctx);
    }
}

#endif

void close_and_free_remote(EV_P_ struct remote_ctx *ctx)
{
    if (ctx != NULL) {
        ev_timer_stop(EV_A_ & ctx->watcher);
        ev_io_stop(EV_A_ & ctx->io);
        if (ctx->src_fd != 0) {
            close(ctx->src_fd);
        }
        close(ctx->fd);
        free(ctx);
    }
}

static void remote_timeout_cb(EV_P_ ev_timer *watcher, int revents)
{
    struct remote_ctx *remote_ctx = (struct remote_ctx *)(((void *)watcher)
                                                          - sizeof(ev_io));

    if (verbose) {
        LOGI("[udp] connection timeout");
    }

    char *key = hash_key(remote_ctx->addr_header,
                         remote_ctx->addr_header_len, &remote_ctx->src_addr);
    cache_remove(remote_ctx->server_ctx->conn_cache, key);
}

#ifdef UDPRELAY_REMOTE
static void query_resolve_cb(struct sockaddr *addr, void *data)
{
    struct query_ctx *query_ctx = (struct query_ctx *)data;
    struct ev_loop *loop = query_ctx->server_ctx->loop;

    if (verbose) {
        LOGI("[udp] udns resolved");
    }

    query_ctx->query = NULL;

    if (addr == NULL) {
        LOGE("[udp] udns returned an error");
    } else {
        int remotefd = create_remote_socket(addr->sa_family == AF_INET6);
        if (remotefd != -1) {
            setnonblocking(remotefd);
#ifdef SO_BROADCAST
            set_broadcast(remotefd);
#endif
#ifdef SO_NOSIGPIPE
            set_nosigpipe(remotefd);
#endif
#ifdef SET_INTERFACE
            if (query_ctx->server_ctx->iface) {
                setinterface(remotefd, query_ctx->server_ctx->iface);
            }
#endif

            struct remote_ctx *remote_ctx = new_remote(remotefd,
                                                       query_ctx->server_ctx);
            remote_ctx->src_addr = query_ctx->src_addr;
            if (addr->sa_family == AF_INET) {
                memcpy(&(remote_ctx->dst_addr), addr,
                       sizeof(struct sockaddr_in));
            } else if (addr->sa_family == AF_INET6) {
                memcpy(&(remote_ctx->dst_addr), addr,
                       sizeof(struct sockaddr_in6));
            }
            remote_ctx->server_ctx = query_ctx->server_ctx;
            remote_ctx->addr_header_len = query_ctx->addr_header_len;
            memcpy(remote_ctx->addr_header, query_ctx->addr_header,
                   query_ctx->addr_header_len);

            size_t addr_len = get_sockaddr_len((struct sockaddr *)&remote_ctx->dst_addr);
            int s = sendto(remote_ctx->fd, query_ctx->buf, query_ctx->buf_len,
                           0, (struct sockaddr *)&remote_ctx->dst_addr,
                           addr_len);

            if (s == -1) {
                ERROR("[udp] sendto_remote");
                close_and_free_remote(EV_A_ remote_ctx);
            } else {
                // Add to conn cache
                char *key = hash_key(remote_ctx->addr_header,
                                     remote_ctx->addr_header_len,
                                     &remote_ctx->src_addr);
                cache_insert(query_ctx->server_ctx->conn_cache, key,
                             (void *)remote_ctx);

                ev_io_start(EV_A_ & remote_ctx->io);
                ev_timer_start(EV_A_ & remote_ctx->watcher);
            }

        } else {
            ERROR("[udp] bind() error");
        }
    }

    // clean up
    close_and_free_query(EV_A_ query_ctx);
}
#endif

static void remote_recv_cb(EV_P_ ev_io *w, int revents)
{
    struct remote_ctx *remote_ctx = (struct remote_ctx *)w;
    struct server_ctx *server_ctx = remote_ctx->server_ctx;

    // server has been closed
    if (server_ctx == NULL) {
        LOGE("[udp] invalid server");
        close_and_free_remote(EV_A_ remote_ctx);
        return;
    }

    if (verbose) {
        LOGI("[udp] remote receive a packet");
    }

    struct sockaddr src_addr;
    socklen_t src_addr_len = sizeof(src_addr);
    char *buf = malloc(BUF_SIZE);

    // recv
    ssize_t buf_len = recvfrom(remote_ctx->fd, buf, BUF_SIZE, 0, &src_addr,
                               &src_addr_len);

    if (buf_len == -1) {
        // error on recv
        // simply drop that packet
        if (verbose) {
            ERROR("[udp] server_recvfrom");
        }
        goto CLEAN_UP;
    }

#ifdef UDPRELAY_LOCAL
    buf = ss_decrypt_all(BUF_SIZE, buf, &buf_len, server_ctx->method);
    if (buf == NULL) {
        if (verbose) {
            ERROR("[udp] server_ss_decrypt_all");
        }
        goto CLEAN_UP;
    }

    int len = parse_udprealy_header(buf, buf_len, NULL, NULL, NULL);
    if (len == 0) {
        LOGI("[udp] error in parse header");
        // error in parse header
        goto CLEAN_UP;
    }
    // server may return using a different address type other than the type we
    // have used during sending

#if defined(UDPRELAY_TUNNEL) || defined(UDPRELAY_REDIR)
    // Construct packet
    buf_len -= len;
    memmove(buf, buf + len, buf_len);
#else
    // Construct packet
    buf = realloc(buf, buf_len + 3);
    memmove(buf + 3, buf, buf_len);
    memset(buf, 0, 3);
    buf_len += 3;
#endif
#endif

#ifdef UDPRELAY_REMOTE

    unsigned int addr_header_len = remote_ctx->addr_header_len;

    // Construct packet
    buf = realloc(buf, buf_len + addr_header_len);
    memmove(buf + addr_header_len, buf, buf_len);
    memcpy(buf, remote_ctx->addr_header, addr_header_len);
    buf_len += addr_header_len;

    buf = ss_encrypt_all(BUF_SIZE, buf, &buf_len, server_ctx->method);
#endif

    size_t remote_src_addr_len = get_sockaddr_len((struct sockaddr *)&remote_ctx->src_addr);

#ifdef UDPRELAY_REDIR
    size_t remote_dst_addr_len = get_sockaddr_len((struct sockaddr *)&remote_ctx->dst_addr);

    if (remote_ctx->src_fd == 0) {
        int src_sock = socket(remote_ctx->src_addr.ss_family, SOCK_DGRAM, 0);
        if (src_sock < 0) {
            ERROR("[udp] remote_recv_socket");
        }
        int opt = 1;
        if (setsockopt(src_sock, SOL_IP, IP_TRANSPARENT, &opt, sizeof(opt))) {
            ERROR("[udp] remote_recv_setsockopt");
            close(src_sock);
            goto CLEAN_UP;
        }
        if (setsockopt(src_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
            ERROR("[udp] remote_recv_setsockopt");
            close(src_sock);
            goto CLEAN_UP;
        }
        if (bind(src_sock, (struct sockaddr *)&remote_ctx->dst_addr, remote_dst_addr_len) != 0) {
            ERROR("[udp] remote_recv_bind");
            close(src_sock);
            goto CLEAN_UP;
        }
        remote_ctx->src_fd = src_sock;
    }

    int s = sendto(remote_ctx->src_fd, buf, buf_len, 0,
            (struct sockaddr *)&remote_ctx->src_addr, remote_src_addr_len);
    if (s == -1) {
        ERROR("[udp] remote_recv_sendto");
        goto CLEAN_UP;
    }
#else
    int s = sendto(server_ctx->fd, buf, buf_len, 0,
                   (struct sockaddr *)&remote_ctx->src_addr, remote_src_addr_len);
    if (s == -1) {
        ERROR("[udp] remote_recv_sendto");
        goto CLEAN_UP;
    }
#endif

    // handle the UDP packet sucessfully,
    // triger the timer
    ev_timer_again(EV_A_ & remote_ctx->watcher);

 CLEAN_UP:
    free(buf);

}

static void server_recv_cb(EV_P_ ev_io *w, int revents)
{
    struct server_ctx *server_ctx = (struct server_ctx *)w;
    struct sockaddr_storage src_addr;
    memset(&src_addr, 0, sizeof(struct sockaddr_storage));
    char *buf = malloc(BUF_SIZE);

    socklen_t src_addr_len = sizeof(struct sockaddr_storage);
    unsigned int offset = 0;

#ifdef UDPRELAY_REDIR
    char control_buffer[64] = { 0 };
    struct msghdr msg;
    struct iovec iov[1];
    struct sockaddr_storage dst_addr;
    memset(&dst_addr, 0, sizeof(struct sockaddr_storage));

    msg.msg_name = &src_addr;
    msg.msg_namelen = src_addr_len;
    msg.msg_control = control_buffer;
    msg.msg_controllen = sizeof(control_buffer);

    iov[0].iov_base = buf;
    iov[0].iov_len = BUF_SIZE;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    ssize_t buf_len = recvmsg(server_ctx->fd, &msg, 0);
    if (buf_len == -1) {
        ERROR("[udp] server_recvmsg");
        goto CLEAN_UP;
    }

    if (get_dstaddr(&msg, &dst_addr)) {
        LOGE("[udp] unable to get dest addr");
        goto CLEAN_UP;
    }
#else
    ssize_t buf_len =
        recvfrom(server_ctx->fd, buf, BUF_SIZE, 0, (struct sockaddr *)&src_addr,
                 &src_addr_len);

    if (buf_len == -1) {
        // error on recv
        // simply drop that packet
        if (verbose) {
            ERROR("[udp] server_recvfrom");
        }
        goto CLEAN_UP;
    }
#endif

    if (verbose) {
        LOGI("[udp] server receive a packet");
    }

#ifdef UDPRELAY_REMOTE
    buf = ss_decrypt_all(BUF_SIZE, buf, &buf_len, server_ctx->method);
    if (buf == NULL) {
        if (verbose) {
            ERROR("[udp] server_ss_decrypt_all");
        }
        goto CLEAN_UP;
    }
#endif

#ifdef UDPRELAY_LOCAL
#if !defined(UDPRELAY_TUNNEL) && !defined(UDPRELAY_REDIR)
    uint8_t frag = *(uint8_t *)(buf + 2);
    offset += 3;
#endif
#endif

    /*
     *
     * SOCKS5 UDP Request
     * +----+------+------+----------+----------+----------+
     * |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
     * +----+------+------+----------+----------+----------+
     * | 2  |  1   |  1   | Variable |    2     | Variable |
     * +----+------+------+----------+----------+----------+
     *
     * SOCKS5 UDP Response
     * +----+------+------+----------+----------+----------+
     * |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
     * +----+------+------+----------+----------+----------+
     * | 2  |  1   |  1   | Variable |    2     | Variable |
     * +----+------+------+----------+----------+----------+
     *
     * shadowsocks UDP Request (before encrypted)
     * +------+----------+----------+----------+
     * | ATYP | DST.ADDR | DST.PORT |   DATA   |
     * +------+----------+----------+----------+
     * |  1   | Variable |    2     | Variable |
     * +------+----------+----------+----------+
     *
     * shadowsocks UDP Response (before encrypted)
     * +------+----------+----------+----------+
     * | ATYP | DST.ADDR | DST.PORT |   DATA   |
     * +------+----------+----------+----------+
     * |  1   | Variable |    2     | Variable |
     * +------+----------+----------+----------+
     *
     * shadowsocks UDP Request and Response (after encrypted)
     * +-------+--------------+
     * |   IV  |    PAYLOAD   |
     * +-------+--------------+
     * | Fixed |   Variable   |
     * +-------+--------------+
     *
     */

#ifdef UDPRELAY_REDIR
    char addr_header[256] = { 0 };
    int addr_header_len = 0;

    if (dst_addr.ss_family == AF_INET) {
        struct sockaddr_in *addr = (struct sockaddr_in *)&dst_addr;
        size_t addr_len = sizeof(struct in_addr);
        addr_header[addr_header_len++] = 1;
        memcpy(addr_header + addr_header_len, &addr->sin_addr, addr_len);
        addr_header_len += addr_len;
        memcpy(addr_header + addr_header_len, &addr->sin_port, 2);
        addr_header_len += 2;
    } else if (dst_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)&dst_addr;
        size_t addr_len = sizeof(struct in6_addr);
        addr_header[addr_header_len++] = 4;
        memcpy(addr_header + addr_header_len, &addr->sin6_addr, addr_len);
        addr_header_len += addr_len;
        memcpy(addr_header + addr_header_len, &addr->sin6_port, 2);
        addr_header_len += 2;
    } else {
        LOGE("[udp] failed to parse tproxy addr");
        goto CLEAN_UP;
    }

    // reconstruct the buffer
    buf = realloc(buf, buf_len + addr_header_len);
    memmove(buf + addr_header_len, buf, buf_len);
    memcpy(buf, addr_header, addr_header_len);
    buf_len += addr_header_len;

#elif UDPRELAY_TUNNEL

    char addr_header[256] = { 0 };
    char *host = server_ctx->tunnel_addr.host;
    char *port = server_ctx->tunnel_addr.port;
    uint16_t port_num = (uint16_t)atoi(port);
    uint16_t port_net_num = htons(port_num);
    int addr_header_len = 0;

    struct cork_ip ip;
    if (cork_ip_init(&ip, host) != -1) {
        if (ip.version == 4) {
            // send as IPv4
            struct in_addr host_addr;
            int host_len = sizeof(struct in_addr);

            if (dns_pton(AF_INET, host, &host_addr) == -1) {
                FATAL("IP parser error");
            }
            addr_header[addr_header_len++] = 1;
            memcpy(addr_header + addr_header_len, &host_addr, host_len);
            addr_header_len += host_len;
        } else if (ip.version == 6) {
            // send as IPv6
            struct in6_addr host_addr;
            int host_len = sizeof(struct in6_addr);

            if (dns_pton(AF_INET6, host, &host_addr) == -1) {
                FATAL("IP parser error");
            }
            addr_header[addr_header_len++] = 4;
            memcpy(addr_header + addr_header_len, &host_addr, host_len);
            addr_header_len += host_len;
        } else {
            FATAL("IP parser error");
        }
    } else {
        // send as domain
        int host_len = strlen(host);

        addr_header[addr_header_len++] = 3;
        addr_header[addr_header_len++] = host_len;
        memcpy(addr_header + addr_header_len, host, host_len);
        addr_header_len += host_len;
    }
    memcpy(addr_header + addr_header_len, &port_net_num, 2);
    addr_header_len += 2;

    // reconstruct the buffer
    buf = realloc(buf, buf_len + addr_header_len);
    memmove(buf + addr_header_len, buf, buf_len);
    memcpy(buf, addr_header, addr_header_len);
    buf_len += addr_header_len;

#else

    char host[256] = { 0 };
    char port[64] = { 0 };
    struct sockaddr_storage storage;
    memset(&storage, 0, sizeof(struct sockaddr_storage));

    int addr_header_len = parse_udprealy_header(buf + offset,
                                                buf_len - offset, host, port,
                                                &storage);
    if (addr_header_len == 0) {
        // error in parse header
        goto CLEAN_UP;
    }
    char *addr_header = buf + offset;
#endif

    char *key = hash_key(addr_header, addr_header_len, &src_addr);
    struct cache *conn_cache = server_ctx->conn_cache;

    struct remote_ctx *remote_ctx = NULL;
    cache_lookup(conn_cache, key, (void *)&remote_ctx);

    if (remote_ctx != NULL) {
        if (memcmp(&src_addr, &remote_ctx->src_addr, sizeof(src_addr))
            || remote_ctx->addr_header_len != addr_header_len
            || memcmp(addr_header, remote_ctx->addr_header, addr_header_len) != 0) {
            remote_ctx = NULL;
        }
    }

    if (remote_ctx == NULL) {
        if (verbose) {
#ifdef UDPRELAY_REDIR
            char src[SS_ADDRSTRLEN];
            char dst[SS_ADDRSTRLEN];
            strcpy(src, get_addr_str((struct sockaddr *)&src_addr));
            strcpy(dst, get_addr_str((struct sockaddr *)&dst_addr));
            LOGI("[udp] cache miss: %s <-> %s", dst, src);
#else
            LOGI("[udp] cache miss: %s:%s <-> %s", host, port,
                 get_addr_str((struct sockaddr *)&src_addr));
#endif
        }
    } else {
        if (verbose) {
#ifdef UDPRELAY_REDIR
            char src[SS_ADDRSTRLEN];
            char dst[SS_ADDRSTRLEN];
            strcpy(src, get_addr_str((struct sockaddr *)&src_addr));
            strcpy(dst, get_addr_str((struct sockaddr *)&dst_addr));
            LOGI("[udp] cache hit: %s <-> %s", dst, src);
#else
            LOGI("[udp] cache hit: %s:%s <-> %s", host, port,
                 get_addr_str((struct sockaddr *)&src_addr));
#endif
        }
    }

#ifdef UDPRELAY_LOCAL

#if !defined(UDPRELAY_TUNNEL) && !defined(UDPRELAY_REDIR)
    if (frag) {
        LOGE("[udp] drop a message since frag is not 0, but %d", frag);
        goto CLEAN_UP;
    }
#endif

    const struct sockaddr *remote_addr = server_ctx->remote_addr;
    const int remote_addr_len = server_ctx->remote_addr_len;

    if (remote_ctx == NULL) {
        // Bind to any port
        int remotefd = create_remote_socket(remote_addr->sa_family == AF_INET6);
        if (remotefd < 0) {
            ERROR("[udp] udprelay bind() error");
            goto CLEAN_UP;
        }
        setnonblocking(remotefd);

#ifdef SO_NOSIGPIPE
        set_nosigpipe(remotefd);
#endif
#ifdef SET_INTERFACE
        if (server_ctx->iface) {
            setinterface(remotefd, server_ctx->iface);
        }
#endif

        // Init remote_ctx
        remote_ctx = new_remote(remotefd, server_ctx);
        remote_ctx->src_addr = src_addr;
#ifdef UDPRELAY_REDIR
        memcpy(&(remote_ctx->dst_addr), &dst_addr, get_sockaddr_len((struct sockaddr *)&dst_addr));
#endif
        remote_ctx->addr_header_len = addr_header_len;
        memcpy(remote_ctx->addr_header, addr_header, addr_header_len);

        // Add to conn cache
        cache_insert(conn_cache, key, (void *)remote_ctx);

        // Start remote io
        ev_io_start(EV_A_ & remote_ctx->io);
        ev_timer_start(EV_A_ & remote_ctx->watcher);
    }

    if (offset > 0) {
        buf_len -= offset;
        memmove(buf, buf + offset, buf_len);
    }

    buf = ss_encrypt_all(BUF_SIZE, buf, &buf_len, server_ctx->method);

    int s = sendto(remote_ctx->fd, buf, buf_len, 0, remote_addr, remote_addr_len);

    if (s == -1) {
        ERROR("[udp] sendto_remote");
    }

#else

    if (remote_ctx == NULL) {
        if (storage.ss_family == AF_INET || storage.ss_family == AF_INET6) {
            int remotefd = create_remote_socket(storage.ss_family == AF_INET6);
            if (remotefd != -1) {
                setnonblocking(remotefd);
#ifdef SO_BROADCAST
                set_broadcast(remotefd);
#endif
#ifdef SO_NOSIGPIPE
                set_nosigpipe(remotefd);
#endif
#ifdef SET_INTERFACE
                if (server_ctx->iface) {
                    setinterface(remotefd, server_ctx->iface);
                }
#endif
                struct remote_ctx *remote_ctx = new_remote(remotefd, server_ctx);
                remote_ctx->src_addr = src_addr;
                remote_ctx->dst_addr = storage;
                remote_ctx->server_ctx = server_ctx;
                remote_ctx->addr_header_len = addr_header_len;
                memcpy(remote_ctx->addr_header, addr_header, addr_header_len);

                size_t addr_len = get_sockaddr_len((struct sockaddr *)&remote_ctx->dst_addr);
                int s = sendto(remote_ctx->fd, buf + addr_header_len,
                               buf_len - addr_header_len, 0,
                               (struct sockaddr *)&remote_ctx->dst_addr,
                               addr_len);

                if (s == -1) {
                    ERROR("[udp] sendto_remote");
                    close_and_free_remote(EV_A_ remote_ctx);
                } else {
                    // Add to conn cache
                    char *key = hash_key(remote_ctx->addr_header,
                                         remote_ctx->addr_header_len,
                                         &remote_ctx->src_addr);
                    cache_insert(server_ctx->conn_cache, key,
                                 (void *)remote_ctx);

                    ev_io_start(EV_A_ & remote_ctx->io);
                    ev_timer_start(EV_A_ & remote_ctx->watcher);
                }
            } else {
                ERROR("[udp] bind() error");
            }
        } else {
            struct addrinfo hints;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_protocol = IPPROTO_UDP;

            struct query_ctx *query_ctx = new_query_ctx(buf + addr_header_len,
                                                        buf_len -
                                                        addr_header_len);
            query_ctx->server_ctx = server_ctx;
            query_ctx->addr_header_len = addr_header_len;
            query_ctx->src_addr = src_addr;
            memcpy(query_ctx->addr_header, addr_header, addr_header_len);

            struct ResolvQuery *query = resolv_query(host, query_resolve_cb,
                                                     NULL, query_ctx,
                                                     htons(atoi(port)));
            if (query == NULL) {
                ERROR("[udp] unable to create DNS query");
                close_and_free_query(EV_A_ query_ctx);
                goto CLEAN_UP;
            }
            query_ctx->query = query;
        }
    } else {
        size_t addr_len = get_sockaddr_len((struct sockaddr *)&remote_ctx->dst_addr);
        int s = sendto(remote_ctx->fd, buf + addr_header_len,
                       buf_len - addr_header_len, 0,
                       (struct sockaddr *)&remote_ctx->dst_addr, addr_len);

        if (s == -1) {
            ERROR("[udp] sendto_remote");
        }
    }
#endif

 CLEAN_UP:
    free(buf);
}

void free_cb(void *element)
{
    struct remote_ctx *remote_ctx = (struct remote_ctx *)element;

    if (verbose) {
        LOGI("[udp] one connection freed");
    }

    close_and_free_remote(EV_DEFAULT, remote_ctx);
}

int init_udprelay(const char *server_host, const char *server_port,
#ifdef UDPRELAY_LOCAL
                  const struct sockaddr *remote_addr, const int remote_addr_len,
#ifdef UDPRELAY_TUNNEL
                  const ss_addr_t tunnel_addr,
#endif
#endif
                  int method, int timeout, const char *iface)
{
    // Inilitialize ev loop
    struct ev_loop *loop = EV_DEFAULT;

    // Inilitialize cache
    struct cache *conn_cache;
    cache_create(&conn_cache, MAX_UDP_CONN_NUM, free_cb);

    //////////////////////////////////////////////////
    // Setup server context

    // Bind to port
    int serverfd = create_server_socket(server_host, server_port);
    if (serverfd < 0) {
        FATAL("[udp] bind() error");
    }
    setnonblocking(serverfd);

    struct server_ctx *server_ctx = new_server_ctx(serverfd);
#ifdef UDPRELAY_REMOTE
    server_ctx->loop = loop;
#endif
    server_ctx->timeout = min(timeout, MAX_UDP_TIMEOUT);
    server_ctx->method = method;
    server_ctx->iface = iface;
    server_ctx->conn_cache = conn_cache;
#ifdef UDPRELAY_LOCAL
    server_ctx->remote_addr = remote_addr;
    server_ctx->remote_addr_len = remote_addr_len;
#ifdef UDPRELAY_TUNNEL
    server_ctx->tunnel_addr = tunnel_addr;
#endif
#endif

    ev_io_start(loop, &server_ctx->io);

    server_ctx_list[server_num++] = server_ctx;

    return 0;
}

int server_init_udprelay(const char *server_host, const char *server_port, int method, int timeout, const char *iface) {
    // Inilitialize ev loop
    struct ev_loop *loop = EV_DEFAULT;

    // Inilitialize cache
    struct cache *conn_cache;
    cache_create(&conn_cache, MAX_UDP_CONN_NUM, free_cb);

    //////////////////////////////////////////////////
    // Setup server context

    // Bind to port
    int serverfd = create_server_socket(server_host, server_port);
    if (serverfd < 0) {
        FATAL("[udp] bind() error");
    }
    setnonblocking(serverfd);

    struct server_ctx *server_ctx = new_server_ctx(serverfd);
    // server_ctx->loop = loop;
    server_ctx->timeout = min(timeout, MAX_UDP_TIMEOUT);
    server_ctx->method = method;
    server_ctx->iface = iface;
    server_ctx->conn_cache = conn_cache;

    ev_io_start(loop, &server_ctx->io);

    server_ctx_list[server_num++] = server_ctx;

    return 0;
}

void free_udprelay()
{
    struct ev_loop *loop = EV_DEFAULT;
    while (server_num-- > 0) {
        struct server_ctx *server_ctx = server_ctx_list[server_num];
        ev_io_stop(loop, &server_ctx->io);
        close(server_ctx->fd);
        cache_delete(server_ctx->conn_cache, 0);
        free(server_ctx);
        server_ctx_list[server_num] = NULL;
    }
}
