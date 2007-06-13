/*
Copyright (c) 2007 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>

#include "ahcpd.h"
#include "config.h"

#define QUERY 0
#define REPLY 1

#define BUFFER_SIZE 1500

struct timeval now;
const struct timeval zero = {0, 0};

static volatile sig_atomic_t exiting = 0;
unsigned char buf[BUFFER_SIZE];
unsigned int data_origin = 0, data_expires = 0, data_age_origin = 0;
int nodns = 0;
char *config_script = "/usr/local/bin/ahcp-config.sh";
int debug_level = 1;

struct network {
    char *ifname;
    int ifindex;
    struct timeval query_time;
    struct timeval reply_time;
};

#define MAXNETWORKS 20
struct network networks[MAXNETWORKS];
int numnetworks;
char *interfaces[MAXNETWORKS + 1];

static void init_signals(void);
static void set_timeout(int i, int which,
                        int msecs, int override);
int ahcp_socket(int port);
int ahcp_recv(int s, void *buf, int buflen, struct sockaddr *sin, int slen);
int ahcp_send(int s,
              const void *buf, int buflen,
              const struct sockaddr *sin, int slen);
int timeval_compare(const struct timeval *s1, const struct timeval *s2);
void timeval_min(struct timeval *d, const struct timeval *s);
void timeval_min_sec(struct timeval *d, int secs);
void timeval_minus(struct timeval *d,
                   const struct timeval *s1, const struct timeval *s2);

static int
time_broken(int nowsecs)
{
    return nowsecs < 1000000000;
}

static int
valid(int nowsecs, int origin, int expires, int age)
{
    if(age >= expires - origin)
        return 0;
    if(time_broken(nowsecs))
        return expires - origin - age;
    if(nowsecs >= expires)
        return 0;
    return MIN(expires - origin - age, nowsecs - expires);
}

int
main(int argc, char **argv)
{
    char *multicast = "ff02::cca6:c0f9:e182:5359";
    unsigned int port = 5359;
    char *authority = NULL;
    struct in6_addr group;
    struct ipv6_mreq mreq;
    struct sockaddr_in6 sin6;
    int fd, rc, s, i, j;
    unsigned int seed;
    int dummy = 0;

    i = 1;
    while(i < argc && argv[i][0] == '-') {
        if(strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else if(strcmp(argv[i], "-m") == 0) {
            i++;
            if(i >= argc) goto usage;
            multicast = argv[i];
            i++;
        } else if(strcmp(argv[i], "-p") == 0) {
            i++;
            if(i >= argc) goto usage;
            port = atoi(argv[i]);
            if(port <= 0 || port > 0xFFFF)
                goto usage;
            i++;
        } else if(strcmp(argv[i], "-a") == 0) {
            i++;
            if(i >= argc) goto usage;
            authority = argv[i];
            i++;
        } else if(strcmp(argv[i], "-n") == 0) {
            dummy = 1;
            i++;
        } else if(strcmp(argv[i], "-N") == 0) {
            nodns = 1;
            i++;
        } else if(strcmp(argv[i], "-c") == 0) {
            i++;
            if(i >= argc) goto usage;
            config_script = argv[i];
            i++;
        } else if(strcmp(argv[i], "-d") == 0) {
            i++;
            if(i >= argc) goto usage;
            debug_level = atoi(argv[i]);
            i++;
        } else {
            goto usage;
        }
    }

    if(argc <= i)
        goto usage;

    for(j = i; j < argc; j++) {
        if(j - i >= MAXNETWORKS) {
            fprintf(stderr, "Too many interfaces.\n");
            exit(1);
        }
        networks[j - i].ifname = argv[j];
        networks[j - i].ifindex = if_nametoindex(argv[j]);
        if(networks[j - i].ifindex <= 0) {
            fprintf(stderr, "Unknown interface %s.\n", argv[j]);
            exit(1);
        }
        networks[j - i].query_time = zero;
        networks[j - i].reply_time = zero;
        interfaces[j - i] = argv[j];
    }
    numnetworks = j - i;
    interfaces[j - i] = NULL;

    rc = inet_pton(AF_INET6, multicast, &group);
    if(rc <= 0)
        goto usage;

    if(authority) {
        fd = open(authority, O_RDONLY);
        if(fd < 0) {
            perror("open(authority)");
            exit(1);
        }
        rc = read(fd, buf, BUFFER_SIZE);
        if(rc < 0) {
            perror("read(authority)");
            exit(1);
        }

        rc = accept_data(buf, rc, interfaces, dummy);
        if(rc < 0) {
            fprintf(stderr, "Couldn't configure from authority data.\n");
            exit(1);
        }

        close(fd);
    }

    gettimeofday(&now, NULL);

    fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0) {
        perror("open(random)");
        seed = now.tv_sec ^ now.tv_usec;
    } else {
        rc = read(fd, &seed, sizeof(unsigned int));
        if(rc < sizeof(unsigned int)) {
            perror("read(random)");
            exit(1);
        }
        close(fd);
    }
    srandom(seed);

    s = ahcp_socket(port);
    if(s < 0) {
        perror("ahcp_socket");
        exit(1);
    }

    for(i = 0; i < numnetworks; i++) {
        memset(&mreq, 0, sizeof(mreq));
        memcpy(&mreq.ipv6mr_multiaddr, &group, 16);
        mreq.ipv6mr_interface = networks[i].ifindex;
        rc = setsockopt(s, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                        (char*)&mreq, sizeof(mreq));
        if(rc < 0) {
            perror("setsockopt(IPV6_JOIN_GROUP)");
            exit(1);
        }
    }

    init_signals();

    if(authority) {
        set_timeout(-1, QUERY, -1, 1);
        set_timeout(-1, REPLY, 5000, 1);
    } else {
        set_timeout(-1, QUERY, 5000, 1);
        set_timeout(-1, REPLY, -1, 1);
    }

    if(debug_level >= 2)
        printf("Entering main loop.\n");

    while(1) {
        struct timeval tv = {0, 0};
        fd_set readfds;

        FD_ZERO(&readfds);
        for(i = 0; i < numnetworks; i++) {
            timeval_min(&tv, &networks[i].query_time);
            timeval_min(&tv, &networks[i].reply_time);
        }
        if(!authority && data_age_origin > 0) {
            int data_age = now.tv_sec - data_age_origin;
            int valid_for =
                valid(now.tv_sec, data_origin, data_expires, data_age);
            /* Wake up 50 seconds before the data expires to send a query */
            if(valid_for >= 50)
                timeval_min_sec(&tv, now.tv_sec + valid_for - 50);
            else if(valid_for > 0)
                timeval_min_sec(&tv, now.tv_sec + valid_for);
        }

        assert(tv.tv_sec != 0);
        if(timeval_compare(&tv, &now) > 0) {
            timeval_minus(&tv, &tv, &now);
            if(time_broken(now.tv_sec)) {
                /* If our clock is broken, it's likely someone (NTP?) is
                   going to step the clock.  Wake up soon just in case. */
                timeval_min_sec(&tv, 30);
            }

            FD_SET(s, &readfds);
            if(debug_level >= 3)
                printf("Sleeping for %d.%03ds.\n",
                       (int)tv.tv_sec, (int)(tv.tv_usec / 1000));
            rc = select(s + 1, &readfds, NULL, NULL, &tv);
            if(rc < 0 && errno != EINTR) {
                perror("select");
                sleep(5);
                continue;
            }
        }

        gettimeofday(&now, NULL);

        if(exiting)
            break;

        if(FD_ISSET(s, &readfds)) {
            rc = ahcp_recv(s, buf, BUFFER_SIZE,
                           (struct sockaddr*)&sin6, sizeof(sin6));
            if(rc < 0) {
                if(errno != EAGAIN && errno != EINTR) {
                    perror("recv");
                    sleep(5);
                }
                continue;
            }
            for(i = 0; i < numnetworks; i++) {
                if(networks[i].ifindex == sin6.sin6_scope_id)
                    break;
            }
            if(i >= numnetworks) {
                fprintf(stderr, "Received packet on unknown network.\n");
                continue;
            }

            if(rc < 4) {
                fprintf(stderr, "Truncated packet.\n");
                continue;
            }

            if(buf[0] != 43) {
                fprintf(stderr, "Incorrect magic.\n");
                continue;
            }

            if(buf[1] != 0) {
                fprintf(stderr, "Incorrect version.\n");
                continue;
            }

            if(buf[2] == 0) {
                /* Query */
                if(debug_level >= 2)
                    printf("Received AHCP query.\n");
                if(config_data)
                    set_timeout(i, REPLY, 3000, 0);
            } else if(buf[2] == 1) {
                /* Reply */
                unsigned int origin, expires;
                unsigned short age, len;
                if(rc < 16) {
                    fprintf(stderr, "Truncated AHCP packet.\n");
                    continue;
                }
                if(debug_level >= 2)
                    printf("Received AHCP reply.\n");
                memcpy(&origin, buf + 4, 4);
                origin = ntohl(origin);
                memcpy(&expires, buf + 8, 4);
                expires = ntohl(expires);
                memcpy(&age, buf + 16, 2);
                age = ntohs(age);
                memcpy(&len, buf + 18, 2);
                len = ntohs(len);
                if(rc < len + 20) {
                    fprintf(stderr, "Truncated AHCP packet.\n");
                    continue;
                }

                if(!valid(now.tv_sec, origin, expires, age)) {
                    if(age > 0 && config_data) {
                        /* The person sending stale data is not
                           authoritative. */
                        set_timeout(i, REPLY, 10000, 0);
                    }
                    continue;
                }

                if(authority)
                    continue;

                if(valid(now.tv_sec, origin, expires, age) &&
                   (!config_data || origin > data_origin)) {
                    /* More fresh than what we've got */
                    rc = accept_data(buf + 20, len, interfaces, dummy);
                    if(rc >= 0) {
                        data_origin = origin;
                        data_expires = expires;
                        data_age_origin = now.tv_sec - age;
                        if(data_age_origin > origin)
                            data_age_origin = data_origin;
                        set_timeout(-1, QUERY, -1, 1);
                        if(rc > 0) {
                            /* Different from what we had, flood it further */
                            set_timeout(-1, REPLY, 3000, 0);
                        }
                    }
                }
            } else {
                fprintf(stderr, "Unknown message type %d\n", buf[2]);
            }
        }

        if(!authority && config_data) {
            int valid_for =
                valid(now.tv_sec, data_origin, data_expires,
                      now.tv_sec - data_age_origin);
            if(!valid_for) {
                /* Our data expired */
                if(debug_level >= 2)
                    printf("AHCP data expired.\n");
                unaccept_data(interfaces, dummy);
                data_expires = data_origin = data_age_origin = 0;
                set_timeout(-1, QUERY, 3000, 0);
            } else if(valid_for <= 50) {
                /* Our data is going to expire soon */
                if(debug_level >= 2)
                    printf("AHCP data about to expire.\n");
                set_timeout(-1, QUERY, 10000, 0);
            }
        }

        for(i = 0; i < numnetworks; i++) {
            if(networks[i].reply_time.tv_sec > 0 &&
               timeval_compare(&networks[i].reply_time, &now) <= 0) {
                unsigned int origin, expires;
                unsigned short age, len;

                if(!config_data) {
                    fprintf(stderr,
                            "Attempted to send AHCP reply "
                            "while unconfigured.\n");
                    set_timeout(i, REPLY, -1, 1);
                    continue;
                }

                if(authority) {
                    origin = htonl(now.tv_sec);
                    expires = htonl(now.tv_sec + 1200);
                    age = htons(0);
                } else {
                    origin = htonl(data_origin);
                    expires = htonl(data_expires);
                    age = htons(now.tv_sec - data_age_origin + 1);
                }
                len = htons(data_len);
                buf[0] = 43;
                buf[1] = 0;
                buf[2] = 1;
                buf[3] = 0;
                memcpy(buf + 4, &origin, 4);
                memcpy(buf + 8, &expires, 4);
                memcpy(buf + 16, &age, 2);
                memcpy(buf + 18, &len, 2);
                memcpy(buf + 20, config_data, data_len);

                memset(&sin6, 0, sizeof(sin6));
                sin6.sin6_family = AF_INET6;
                memcpy(&sin6.sin6_addr, &group, 16);
                sin6.sin6_port = htons(port);
                sin6.sin6_scope_id = networks[i].ifindex;
                if(debug_level >= 2)
                    printf("Sending AHCP reply on %s.\n", networks[i].ifname);
                rc = ahcp_send(s, buf, 20 + data_len,
                               (struct sockaddr*)&sin6, sizeof(sin6));
                if(rc < 0)
                    perror("ahcp_send");
                if(authority)
                    set_timeout(i, REPLY, 120 * 1000, 1);
                else
                    set_timeout(i, REPLY, 300 * 1000, 1);
            }

            if(networks[i].query_time.tv_sec > 0 &&
               timeval_compare(&networks[i].query_time, &now) <= 0) {
                buf[0] = 43;
                buf[1] = 0;
                buf[2] = 0;
                buf[3] = 0;

                memset(&sin6, 0, sizeof(sin6));
                sin6.sin6_family = AF_INET6;
                memcpy(&sin6.sin6_addr, &group, 16);
                sin6.sin6_port = htons(port);
                sin6.sin6_scope_id = networks[i].ifindex;
                if(debug_level >= 2)
                    printf("Sending AHCP request on %s.\n", networks[i].ifname);
                rc = ahcp_send(s, buf, 4,
                               (struct sockaddr*)&sin6, sizeof(sin6));
                if(rc < 0)
                    perror("ahcp_send");
                if(authority)
                    set_timeout(i, QUERY, -1, 1);
                else if(config_data)
                    set_timeout(i, QUERY, 600 * 1000, 1);
                else
                    set_timeout(i, QUERY, 15000, 1);
            }
        }
    }
    if(config_data) {
        rc = unaccept_data(interfaces, dummy);
        if(rc < 0) {
            fprintf(stderr, "Couldn't unconfigure!\n");
            exit(1);
        }
    }
    return 0;

 usage:
    fprintf(stderr,
            "Syntax: ahcpd "
            "[-m group] [-p port] [-a authority_file] [-n] [-N] [-c script]\n"
            "              "
            "interface...\n");
    exit(1);
}

static void
set_timeout(int net, int which, int msecs, int override)
{
    if(net < 0) {
        int i;
        for(i = 0; i < numnetworks; i++)
            set_timeout(i, which, msecs, override);
    } else {
        struct timeval *tv;
        int ms = msecs / 2 + random() % msecs;
        if(which == QUERY)
            tv = &networks[net].query_time;
        else
            tv = &networks[net].reply_time;
        /* (0, 0) represents never */
        if(override || tv->tv_sec == 0 || tv->tv_sec > now.tv_sec + ms / 1000) {
            if(msecs < 0) {
                tv->tv_usec = 0;
                tv->tv_sec = 0;
            } else {
                tv->tv_usec = (now.tv_usec + ms * 1000) % 1000000;
                tv->tv_sec = now.tv_sec + (now.tv_usec / 1000 + ms) / 1000;
            }
        }
    }
}

static void
sigexit(int signo)
{
    exiting = 1;
}

static void
init_signals(void)
{
    struct sigaction sa;
    sigset_t ss;

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

int
ahcp_socket(int port)
{
    struct sockaddr_in6 sin6;
    int s, rc;
    int saved_errno;
    int one = 1, zero = 0, twofiftyfive = 255;

    s = socket(PF_INET6, SOCK_DGRAM, 0);
    if(s < 0)
        return -1;

    rc = setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    if(rc < 0)
        goto fail;

    rc = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(rc < 0)
        goto fail;

    rc = setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                    &zero, sizeof(zero));
    if(rc < 0)
        goto fail;

    rc = setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS,
                    &twofiftyfive, sizeof(twofiftyfive));
    if(rc < 0)
        goto fail;

    rc = setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                    &twofiftyfive, sizeof(twofiftyfive));
    if(rc < 0)
        goto fail;

    rc = fcntl(s, F_GETFL, 0);
    if(rc < 0)
        goto fail;

    rc = fcntl(s, F_SETFL, (rc | O_NONBLOCK));
    if(rc < 0)
        goto fail;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(port);
    rc = bind(s, (struct sockaddr*)&sin6, sizeof(sin6));
    if(rc < 0)
        goto fail;

    return s;

 fail:
    saved_errno = errno;
    close(s);
    errno = saved_errno;
    return -1;
}

int
ahcp_recv(int s, void *buf, int buflen, struct sockaddr *sin, int slen)
{
    struct iovec iovec;
    struct msghdr msg;
    int rc;

    memset(&msg, 0, sizeof(msg));
    iovec.iov_base = buf;
    iovec.iov_len = buflen;
    msg.msg_name = sin;
    msg.msg_namelen = slen;
    msg.msg_iov = &iovec;
    msg.msg_iovlen = 1;

    rc = recvmsg(s, &msg, 0);
    return rc;
}

int
ahcp_send(int s,
          const void *buf, int buflen,
          const struct sockaddr *sin, int slen)
{
    struct iovec iovec[1];
    struct msghdr msg;
    int rc;

    iovec[0].iov_base = (void*)buf;
    iovec[0].iov_len = buflen;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (struct sockaddr*)sin;
    msg.msg_namelen = slen;
    msg.msg_iov = iovec;
    msg.msg_iovlen = 1;
    rc = sendmsg(s, &msg, 0);
    return rc;
}

int
timeval_compare(const struct timeval *s1, const struct timeval *s2)
{
    if(s1->tv_sec < s2->tv_sec)
        return -1;
    else if(s1->tv_sec > s2->tv_sec)
        return 1;
    else if(s1->tv_usec < s2->tv_usec)
        return -1;
    else if(s1->tv_usec > s2->tv_usec)
        return 1;
    else
        return 0;
}

/* {0, 0} represents infinity */
void
timeval_min(struct timeval *d, const struct timeval *s)
{
    if(s->tv_sec == 0)
        return;

    if(d->tv_sec == 0 || timeval_compare(d, s) > 0) {
        *d = *s;
    }
}

void
timeval_minus(struct timeval *d,
              const struct timeval *s1, const struct timeval *s2)
{
    if(s1->tv_usec > s2->tv_usec) {
        d->tv_usec = s1->tv_usec - s2->tv_usec;
        d->tv_sec = s1->tv_sec - s2->tv_sec;
    } else {
        d->tv_usec = s1->tv_usec + 1000000 - s2->tv_usec;
        d->tv_sec = s1->tv_sec - s2->tv_sec - 1;
    }
}

void
timeval_min_sec(struct timeval *d, int secs)
{
    if(d->tv_sec == 0 || d->tv_sec > secs) {
        d->tv_sec = secs;
        d->tv_usec = random() % 1000000;
    }
}
