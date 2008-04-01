/*
Copyright (c) 2007, 2008 by Juliusz Chroboczek

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
#include "constants.h"
#include "message.h"
#include "config.h"
#include "lease.h"

#define QUERY 0
#define REPLY 1
#define STATEFUL_REQUEST 2
#define STATEFUL_EXPIRE 3
#define CHECK_NETWORKS 4

struct timeval now;
const struct timeval zero = {0, 0};

static volatile sig_atomic_t exiting = 0, dumping = 0, changed = 0;
struct in6_addr protocol_group;
int protocol_socket = -1;
char *authority = NULL;
unsigned char unique_id[16];
char *unique_id_file = "/var/lib/ahcpd-unique-id";
unsigned char buf[BUFFER_SIZE];
unsigned int data_origin = 0, data_expires = 0, data_age_origin = 0;
int nodns = 0, nostate = 0;
char *config_script = "/usr/local/bin/ahcp-config.sh";
int debug_level = 1;
int do_daemonise = 0;
char *logfile = NULL, *pidfile = NULL;

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
struct timeval check_networks_time = {0, 0};
struct timeval stateful_request_time = {0, 0};
struct timeval stateful_expire_time = {0, 0};

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
static int reopen_logfile(void);
static int daemonise();


static int
time_broken(int nowsecs)
{
    return nowsecs < 1200000000;
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
    return MIN(expires - origin - age, expires - nowsecs);
}

#define QUERY_DELAY 1000
#define INITIAL_QUERY_TIMEOUT 2000
#define MAX_QUERY_TIMEOUT 30000
#define STATEFUL_REQUEST_DELAY 8000
#define INITIAL_STATEFUL_REQUEST_TIMEOUT 2000
#define MAX_STATEFUL_REQUEST_TIMEOUT 60000

static int
check_network(struct network *net)
{
    int ifindex, rc;
    struct ipv6_mreq mreq;

    ifindex = if_nametoindex(net->ifname);
    if(ifindex != net->ifindex) {
        net->ifindex = ifindex;
        if(net->ifindex > 0) {
            memset(&mreq, 0, sizeof(mreq));
            memcpy(&mreq.ipv6mr_multiaddr, &protocol_group, 16);
            mreq.ipv6mr_interface = net->ifindex;
            rc = setsockopt(protocol_socket, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                            (char*)&mreq, sizeof(mreq));
            if(rc < 0) {
                perror("setsockopt(IPV6_JOIN_GROUP)");
                net->ifindex = 0;
                goto fail;
            }
            if(authority) {
                set_timeout(-1, QUERY, -1, 1);
                set_timeout(-1, REPLY, 5000, 1);
            } else {
                set_timeout(-1, QUERY, QUERY_DELAY, 1);
                set_timeout(-1, REPLY, -1, 1);
            }
            return 1;
        }
    }
 fail:
    return 0;
}

int
main(int argc, char **argv)
{
    char *multicast = "ff02::cca6:c0f9:e182:5359";
    unsigned int port = 5359;
    struct sockaddr_in6 sin6;
    int fd, rc, i, j, net;
    unsigned int seed;
    int dummy = 0;
    int expires_delay = 3600;
    int query_timeout = INITIAL_QUERY_TIMEOUT;
    int stateful_request_timeout = INITIAL_STATEFUL_REQUEST_TIMEOUT;
    char *lease_dir = NULL;
    unsigned int lease_first = 0, lease_last = 0;
    int selected_stateful_server = -1;
    int current_stateful_server = -1;

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
        } else if(strcmp(argv[i], "-e") == 0) {
            i++;
            if(i >= argc) goto usage;
            expires_delay = atoi(argv[i]);
            if(expires_delay <= 30)
                goto usage;
            i++;
        } else if(strcmp(argv[i], "-n") == 0) {
            dummy = 1;
            i++;
        } else if(strcmp(argv[i], "-N") == 0) {
            nodns = 1;
            i++;
        } else if(strcmp(argv[i], "-s") == 0) {
            nostate = 1;
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
        } else if(strcmp(argv[i], "-i") == 0) {
            i++;
            if(i >= argc) goto usage;
            unique_id_file = argv[i];
            i++;
#ifndef NO_STATEFUL_SERVER
        } else if(strcmp(argv[i], "-S") == 0) {
            unsigned char ipv4[4];
            int rc;
            if(lease_dir)
                goto usage;
            i++;
            if(i >= argc) goto usage;
            rc = inet_pton(AF_INET, argv[i], ipv4);
            if(rc <= 0) goto usage;
            memcpy(&lease_first, ipv4, 4);
            lease_first = ntohl(lease_first);
            i++;
            if(i >= argc) goto usage;
            rc = inet_pton(AF_INET, argv[i], ipv4);
            if(rc <= 0) goto usage;
            memcpy(&lease_last, ipv4, 4);
            lease_last = ntohl(lease_last);
            i++;
            if(i >= argc) goto usage;
            lease_dir = argv[i];
            i++;
#endif
        } else if(strcmp(argv[i], "-D") == 0) {
            do_daemonise = 1;
            i++;
        } else if(strcmp(argv[i], "-L") == 0) {
            i++;
            if(i >= argc) goto usage;
            logfile = argv[i];
            i++;
        } else if(strcmp(argv[i], "-I") == 0) {
            i++;
            if(i >= argc) goto usage;
            pidfile = argv[i];
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
        interfaces[j - i] = argv[j];
    }
    numnetworks = j - i;
    interfaces[j - i] = NULL;

    rc = inet_pton(AF_INET6, multicast, &protocol_group);
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

    if(do_daemonise) {
        if(logfile == NULL)
            logfile = "/var/log/ahcpd.log";
    }

    rc = reopen_logfile();
    if(rc < 0) {
        perror("reopen_logfile()");
        exit(1);
    }

    fd = open("/dev/null", O_RDONLY);
    if(fd < 0) {
        perror("open(null)");
        exit(1);
    }

    rc = dup2(fd, 0);
    if(rc < 0) {
        perror("dup2(null, 0)");
        exit(1);
    }

    close(fd);

    if(do_daemonise) {
        rc = daemonise();
        if(rc < 0) {
            perror("daemonise");
            exit(1);
        }
    }

    if(pidfile) {
        int pfd, len;
        char buf[100];

        len = snprintf(buf, 100, "%lu", (unsigned long)getpid());
        if(len < 0 || len >= 100) {
            perror("snprintf(getpid)");
            exit(1);
        }

        pfd = open(pidfile, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if(pfd < 0) {
            perror("creat(pidfile)");
            exit(1);
        }

        rc = write(pfd, buf, len);
        if(rc < len) {
            perror("write(pidfile)");
            goto fail;
        }

        close(pfd);
    }

    gettimeofday(&now, NULL);

    if(time_broken(now.tv_sec))
        fprintf(stderr,
                "Warning: your clock is fubar (now = %d).\n", (int)now.tv_sec);

    fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0) {
        perror("open(random)");
        seed = now.tv_sec ^ now.tv_usec;
    } else {
        rc = read(fd, &seed, sizeof(unsigned int));
        if(rc < sizeof(unsigned int)) {
            perror("read(random)");
            goto fail;
        }
        close(fd);
    }
    srandom(seed);

    if(unique_id_file && unique_id_file[0] != '\0') {
        fd = open(unique_id_file, O_RDONLY);
        if(fd >= 0) {
            rc = read(fd, unique_id, 16);
            if(rc == 16) {
                close(fd);
                goto unique_id_done;
            }
            close(fd);
        }
    }

    fd = open("/dev/random", O_RDONLY);
    rc = read(fd, unique_id, 16);
    if(rc != 16) {
        perror("read(random)");
        goto fail;
    }
    close(fd);

    if(unique_id_file && unique_id_file[0] != '\0') {
        fd = open(unique_id_file, O_RDWR | O_TRUNC | O_CREAT, 0644);
        if(fd < 0) {
            perror("creat(unique_id)");
        } else {
            rc = write(fd, unique_id, 16);
            if(rc != 16) {
                perror("write(unique_id)");
                unlink(unique_id_file);
            }
            close(fd);
        }
    }
 unique_id_done:

    if(lease_dir) {
        if(time_broken(now.tv_sec)) {
            fprintf(stderr, "Cannot run stateful server with broken clock.\n");
            goto fail;
        }
        rc = lease_init(lease_dir, lease_first, lease_last);
        if(rc < 0) {
            fprintf(stderr, "Couldn't initialise lease database.\n");
            goto fail;
        }
    }

    protocol_socket = ahcp_socket(port);
    if(protocol_socket < 0) {
        perror("ahcp_socket");
        goto fail;
    }

    for(i = 0; i < numnetworks; i++) {
        networks[i].ifname = interfaces[i];
        check_network(&networks[i]);
        if(networks[i].ifindex <= 0) {
            fprintf(stderr, "Warning: unknown interface %s.\n",
                    networks[i].ifname);
            continue;
        }
    }

    init_signals();

    if(authority) {
        if(!nostate && stateful_servers_len >= 16) {
            current_stateful_server = 0;
            set_timeout(-1, STATEFUL_REQUEST, STATEFUL_REQUEST_DELAY, 1);
            stateful_request_timeout = INITIAL_STATEFUL_REQUEST_TIMEOUT;
        }
    }

    set_timeout(-1, CHECK_NETWORKS, 30000, 1);

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
        timeval_min(&tv, &stateful_request_time);
        timeval_min(&tv, &stateful_expire_time);
        timeval_min(&tv, &check_networks_time);

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

            FD_SET(protocol_socket, &readfds);
            if(debug_level >= 3)
                printf("Sleeping for %d.%03ds.\n",
                       (int)tv.tv_sec, (int)(tv.tv_usec / 1000));
            rc = select(protocol_socket + 1, &readfds, NULL, NULL, &tv);
            if(rc < 0 && errno != EINTR) {
                perror("select");
                sleep(5);
                continue;
            }
        }

        gettimeofday(&now, NULL);

        if(exiting)
            break;

        if(dumping) {
            if(config_data) {
                if(authority)
                    printf("Authoritative stateless data.\n");
                else
                    printf("Stateless data valid for %d seconds.\n",
                           valid(now.tv_sec, data_origin, data_expires,
                                 now.tv_sec - data_age_origin));
            } else {
                printf("No stateless data.\n");
            }
            if(ipv4_address[0] != 0)
                printf("Stateful data, valid for %d seconds.\n",
                       (int)(stateful_expire_time.tv_sec - now.tv_sec));
            else
                printf("No stateful data.\n");
            printf("\n");
            fflush(stdout);

            dumping = 0;
        }

        if(changed) {
            for(i = 0; i < numnetworks; i++)
                check_network(&networks[i]);
            set_timeout(-1, CHECK_NETWORKS, 30000, 1);
            rc = reopen_logfile();
            if(rc < 0) {
                perror("reopen_logfile");
                goto fail;
            }
            changed = 0;
        }

        if(FD_ISSET(protocol_socket, &readfds)) {
            rc = ahcp_recv(protocol_socket, buf, BUFFER_SIZE,
                           (struct sockaddr*)&sin6, sizeof(sin6));
            if(rc < 0) {
                if(errno != EAGAIN && errno != EINTR) {
                    perror("recv");
                    sleep(5);
                }
                continue;
            }
            if(IN6_IS_ADDR_LINKLOCAL(&sin6.sin6_addr)) {
                for(net = 0; net < numnetworks; net++) {
                    if(networks[net].ifindex <= 0)
                        continue;
                    if(networks[net].ifindex == sin6.sin6_scope_id)
                        break;
                }
                if(net >= numnetworks) {
                    fprintf(stderr, "Received packet on unknown network.\n");
                    continue;
                }
            } else {
                net = -1;
            }

            if(!validate_packet(buf, rc)) {
                fprintf(stderr,
                        "Received corrupted packet on %s.\n",
                        networks[i].ifname);
            }

            if(buf[2] == AHCP_QUERY) {
                if(net < 0) {
                    fprintf(stderr, "Received non-local query.\n");
                    continue;
                }
                if(debug_level >= 2)
                    printf("Received AHCP query on %s.\n",
                           networks[net].ifname);
                /* Since peers use an initial timeout of 2 seconds,
                   this should be no more than 1.3s (due to jitter). */
                if(config_data)
                    set_timeout(net, REPLY, 1000, 0);
            } else if(buf[2] == AHCP_REPLY) {
                /* Reply */
                unsigned int origin, expires;
                unsigned short age, dlen;
                unsigned char *data;

                if(net < 0) {
                    fprintf(stderr, "Received non-local reply.\n");
                    continue;
                }

                if(debug_level >= 2)
                    printf("Received AHCP reply on %s.\n",
                           networks[net].ifname);

                rc = parse_reply(buf, rc,
                                 &origin, &expires, &age, &data, &dlen);
                if(rc < 0) {
                    fprintf(stderr, "Couldn't parse reply.\n");
                    continue;
                }

                if(origin > expires) {
                    fprintf(stderr,
                            "Received inconsistent AHCP packet "
                            "(origin = %d, expires = %d, now = %d).\n",
                            origin, expires, (int)now.tv_sec);
                    continue;
                }

                if(!time_broken(now.tv_sec)) {
                    if(origin > now.tv_sec + 300) {
                        fprintf(stderr,
                                "Received AHCP packet from the future "
                                "(origin = %d, expires = %d, now = %d).\n"
                                "Perhaps somebody's clock is fubar?\n",
                                origin, expires, (int)now.tv_sec);
                        continue;
                    }
                    if(expires < now.tv_sec - 600) {
                        fprintf(stderr,
                                "Received expired AHCP packet "
                                "(origin = %d, expires = %d, now = %d).\n"
                                "Perhaps somebody's clock is fubar?\n",
                                origin, expires, (int)now.tv_sec);
                        continue;
                    }
                }

                if(!valid(now.tv_sec, origin, expires, age)) {
                    if(age > 0 && config_data) {
                        /* The person sending stale data is not
                           authoritative. */
                        set_timeout(net, REPLY, 10000, 0);
                    }
                    continue;
                }

                if(authority)
                    continue;

                if(valid(now.tv_sec, origin, expires, age) &&
                   (!config_data || origin > data_origin)) {
                    /* More fresh than what we've got */
                    if(config_data && data_changed(data, dlen)) {
                        /* In case someone puts two distinct authoritative
                           configurations on the same network, we want to have
                           some hysteresis.  We ignore different data for
                           at least half its validity interval. */
                        if(valid(now.tv_sec, data_origin, data_expires,
                                 now.tv_sec - data_age_origin) >= 10) {
                            if(valid(now.tv_sec, origin, expires, age) <
                               (expires - origin) / 2)
                                continue;
                        }
                    }
                    rc = accept_data(data, dlen, interfaces, dummy);
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
                        if(!nostate && stateful_servers_len >= 16) {
                            current_stateful_server = 0;
                            set_timeout(-1, STATEFUL_REQUEST,
                                        STATEFUL_REQUEST_DELAY, 1);
                            stateful_request_timeout =
                                INITIAL_STATEFUL_REQUEST_TIMEOUT;
                        } else {
                            current_stateful_server = -1;
                            set_timeout(-1, STATEFUL_REQUEST, -1, 1);
                        }
                    }
                }
            } else if(buf[2] == AHCP_STATEFUL_REQUEST ||
                      buf[2] == AHCP_STATEFUL_RELEASE) {
                unsigned short lease_time, ulen, dlen;
                unsigned char *uid, *data;
                unsigned char suggested_ipv4[4] = {0, 0, 0, 0};
                unsigned char ipv4[4];

                if(debug_level >= 2)
                    printf("Received stateful %s.\n",
                           buf[2] == AHCP_STATEFUL_REQUEST ?
                           "request" : "release");

                if(time_broken(now.tv_sec))
                    continue;

                rc = parse_stateful_packet(buf, rc,
                                           &lease_time, &uid, &ulen,
                                           &data, &dlen);
                if(rc < 0) {
                    fprintf(stderr, "Corrupted stateful request.\n");
                    continue;
                }

                rc = parse_stateful_data(data, dlen, suggested_ipv4);
                if(rc < 0) {
                    fprintf(stderr, "Unacceptable stateful request.\n");
                    continue;
                }

                if(buf[2] == AHCP_STATEFUL_REQUEST) {
                    rc = take_lease(uid, ulen,
                                    suggested_ipv4[0] == 0 ?
                                    NULL : suggested_ipv4,
                                    ipv4, &lease_time);

                    buf[0] = 43;
                    buf[1] = 0;
                    if(rc < 0) {
                        if(debug_level >= 2)
                            printf("Sending stateful NAK.\n");
                        buf[2] = AHCP_STATEFUL_NAK;
                        buf[3] = 0;
                        buf[4] = 0;
                        buf[5] = 0;
                        buf[8 + ulen] = 0;
                        buf[8 + ulen + 1] = 0;
                        rc = ahcp_send(protocol_socket, buf, 8 + ulen + 2,
                                       (struct sockaddr*)&sin6, sizeof(sin6));
                        if(rc < 0) {
                            if(errno == ENETUNREACH)
                                set_timeout(-1, CHECK_NETWORKS, 0, 0);
                            perror("ahcp_send");
                        }
                    } else {
                        int i;
                        if(debug_level >= 2)
                            printf("Sending stateful ACK.\n");
                        lease_time = htons(lease_time);
                        buf[2] = AHCP_STATEFUL_ACK;
                        buf[3] = 0;
                        memcpy(buf + 4, &lease_time, 2);
                        i = 8 + ulen;
                        i += build_stateful_data(buf + i, ipv4);
                        rc = ahcp_send(protocol_socket, buf, i,
                                       (struct sockaddr*)&sin6, sizeof(sin6));
                        if(rc < 0) {
                            if(errno == ENETUNREACH)
                                set_timeout(-1, CHECK_NETWORKS, 0, 0);
                            perror("ahcp_send");
                        }
                    }
                } else {
                    /* Release */
                    release_lease(suggested_ipv4[0] == 0 ?
                                  NULL : suggested_ipv4,
                                  uid, ulen);
                }
            } else if(buf[2] == AHCP_STATEFUL_ACK ||
                      buf[2] == AHCP_STATEFUL_NAK) {
                unsigned short lease_time, ulen, dlen;
                unsigned char *data, *uid;
                int i, found = 0;

                rc = parse_stateful_packet(buf, rc,
                                           &lease_time, &uid, &ulen,
                                           &data, &dlen);

                if(!nostate) {
                    for(i = 0; i < stateful_servers_len / 16; i++) {
                        if(memcmp(stateful_servers + i * 16,
                                  &sin6.sin6_addr, 16) == 0) {
                            found = 1;
                            break;
                        }
                    }
                }

                if(!found) {
                    fprintf(stderr, "Received unexpected stateful reply.\n");
                    continue;
                }

                if(ulen != 16 || memcmp(uid, unique_id, 16) != 0) {
                    fprintf(stderr, "Received stateful reply not for me.\n");
                    continue;
                }

                if(debug_level >= 2)
                    printf("Received stateful %s.\n",
                           buf[2] == AHCP_STATEFUL_ACK ? "ACK" : "NAK");

                if(buf[2] == AHCP_STATEFUL_ACK) {
                    if(lease_time < 4)
                        continue;

                    selected_stateful_server = -1;
                    rc = accept_stateful_data(data, dlen, lease_time,
                                              interfaces);
                    if(rc >= 0) {
                        selected_stateful_server = 0;
                        set_timeout(-1, STATEFUL_EXPIRE, lease_time * 1000, 1);
                        set_timeout(-1, STATEFUL_REQUEST,
                                    MIN(lease_time * 2000 / 3, 60 * 60 * 1000),
                                    1);
                        stateful_request_timeout =
                            INITIAL_STATEFUL_REQUEST_TIMEOUT;
                    } else {
                        set_timeout(-1, STATEFUL_REQUEST,
                                    MAX_STATEFUL_REQUEST_TIMEOUT, 1);
                        stateful_request_timeout = MAX_STATEFUL_REQUEST_TIMEOUT;
                    }
                } else {
                    /* NAK */
                    set_timeout(-1, STATEFUL_REQUEST,
                                MAX_STATEFUL_REQUEST_TIMEOUT, 1);
                    stateful_request_timeout = MAX_STATEFUL_REQUEST_TIMEOUT;
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
                if(ipv4_address[0] != 0) {
                    selected_stateful_server = -1;
                    unaccept_stateful_data(interfaces);
                    set_timeout(-1, STATEFUL_REQUEST, STATEFUL_REQUEST_DELAY, 1);
                    set_timeout(-1, STATEFUL_EXPIRE, -1, 1);
                    stateful_request_timeout = INITIAL_STATEFUL_REQUEST_TIMEOUT;
                }
                current_stateful_server = -1;
                unaccept_data(interfaces, dummy);
                data_expires = data_origin = data_age_origin = 0;
                query_timeout = INITIAL_QUERY_TIMEOUT;
                set_timeout(-1, QUERY, query_timeout, 0);
                set_timeout(-1, STATEFUL_REQUEST, -1, 1);
            } else if(valid_for <= 50) {
                /* Our data is going to expire soon */
                if(debug_level >= 2)
                    printf("AHCP data about to expire.\n");
                set_timeout(-1, QUERY, 10000, 0);
            }
        }

        for(net = 0; net < numnetworks; net++) {
            if(networks[net].reply_time.tv_sec > 0 &&
               timeval_compare(&networks[net].reply_time, &now) <= 0) {
                unsigned int origin, expires;
                unsigned short age, len;

                if(!config_data) {
                    /* This can happen if we expired in the meantime. */
                    set_timeout(net, REPLY, -1, 1);
                    continue;
                }

                if(authority) {
                    origin = htonl(now.tv_sec);
                    expires = htonl(now.tv_sec + expires_delay);
                    age = htons(0);
                } else {
                    origin = htonl(data_origin);
                    expires = htonl(data_expires);
                    age = htons(now.tv_sec - data_age_origin + 1);
                }
                len = htons(data_len);
                buf[0] = 43;
                buf[1] = 0;
                buf[2] = AHCP_REPLY;
                buf[3] = 0;
                memcpy(buf + 4, &origin, 4);
                memcpy(buf + 8, &expires, 4);
                memset(buf + 12, 0, 4);
                memcpy(buf + 16, &age, 2);
                memcpy(buf + 18, &len, 2);
                memcpy(buf + 20, config_data, data_len);

                memset(&sin6, 0, sizeof(sin6));
                sin6.sin6_family = AF_INET6;
                memcpy(&sin6.sin6_addr, &protocol_group, 16);
                sin6.sin6_port = htons(port);
                sin6.sin6_scope_id = networks[net].ifindex;
                if(debug_level >= 2)
                    printf("Sending AHCP reply on %s.\n", networks[net].ifname);
                rc = ahcp_send(protocol_socket, buf, 20 + data_len,
                               (struct sockaddr*)&sin6, sizeof(sin6));
                if(rc < 0) {
                    if(errno == ENETUNREACH)
                        set_timeout(-1, CHECK_NETWORKS, 0, 0);
                    perror("ahcp_send");
                }
                if(!authority)
                    set_timeout(net, REPLY,
                                MAX((data_expires - data_origin) * 125, 120000),
                                1);
                else
                    set_timeout(net, REPLY, MAX(expires_delay * 125, 30000),
                                1);
            }

            if(networks[net].query_time.tv_sec > 0 &&
               timeval_compare(&networks[net].query_time, &now) <= 0) {
                buf[0] = 43;
                buf[1] = 0;
                buf[2] = AHCP_QUERY;
                buf[3] = 0;

                memset(&sin6, 0, sizeof(sin6));
                sin6.sin6_family = AF_INET6;
                memcpy(&sin6.sin6_addr, &protocol_group, 16);
                sin6.sin6_port = htons(port);
                sin6.sin6_scope_id = networks[net].ifindex;
                if(debug_level >= 2)
                    printf("Sending AHCP request on %s.\n",
                           networks[net].ifname);
                rc = ahcp_send(protocol_socket, buf, 4,
                               (struct sockaddr*)&sin6, sizeof(sin6));
                if(rc < 0) {
                    if(errno == ENETUNREACH)
                        set_timeout(-1, CHECK_NETWORKS, 0, 0);
                    perror("ahcp_send");
                }
                if(authority)
                    set_timeout(net, QUERY, -1, 1);
                else if(config_data)
                    set_timeout(net, QUERY, 600 * 1000, 1);
                else {
                    query_timeout = MIN(2 * query_timeout, MAX_QUERY_TIMEOUT);
                    set_timeout(net, QUERY, query_timeout, 1);
                }
            }
        }

        if(stateful_request_time.tv_sec > 0 &&
           timeval_compare(&stateful_request_time, &now) <= 0) {
            unsigned short lease_time = htons(30 * 60);
            unsigned short sixteen = htons(16);
            int rc;
            int server = -1;

            if(selected_stateful_server >= 0)
                server = selected_stateful_server;
            else if(current_stateful_server >= 0)
                server = current_stateful_server;

            if(server < 0 || server >= stateful_servers_len / 16) {
                fprintf(stderr,
                        "Trying to send stateful query with no servers.\n");
                continue;
            }

            buf[0] = 43;
            buf[1] = 0;
            buf[2] = AHCP_STATEFUL_REQUEST;
            buf[3] = 0;
            memcpy(buf + 4, &lease_time, 2);
            memcpy(buf + 6, &sixteen, 2);
            memcpy(buf + 8, unique_id, 16);
            rc = build_stateful_data(buf + 24,
                                     ipv4_address[0] == 0 ?
                                     NULL : ipv4_address);
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            memcpy(&sin6.sin6_addr, stateful_servers + 16 * server, 16);
            sin6.sin6_port = htons(port);
            if(debug_level >= 2)
                printf("Sending stateful request.\n");
            rc = ahcp_send(protocol_socket, buf, 24 + rc,
                           (struct sockaddr*)&sin6, sizeof(sin6));
            if(rc < 0) {
                if(errno == ENETUNREACH)
                    set_timeout(-1, CHECK_NETWORKS, 0, 0);
                perror("ahcp_send");
            }
            stateful_request_timeout = 2 * stateful_request_timeout;
            if(stateful_request_timeout > MAX_STATEFUL_REQUEST_TIMEOUT) {
                current_stateful_server =
                    current_stateful_server % (stateful_servers_len / 16);
                stateful_request_timeout = INITIAL_STATEFUL_REQUEST_TIMEOUT;
            }
            set_timeout(-1, STATEFUL_REQUEST, stateful_request_timeout, 1);
        }

        if(stateful_expire_time.tv_sec > 0 &&
           timeval_compare(&stateful_expire_time, &now) <= 0) {
            if(debug_level >= 2)
                printf("Stateful data expired.\n");
            selected_stateful_server = -1;
            unaccept_stateful_data(interfaces);
            set_timeout(-1, STATEFUL_REQUEST, STATEFUL_REQUEST_DELAY, 1);
            set_timeout(-1, STATEFUL_EXPIRE, -1, 1);
            stateful_request_timeout = INITIAL_STATEFUL_REQUEST_TIMEOUT;
        }

        if(check_networks_time.tv_sec > 0 &&
           timeval_compare(&check_networks_time, &now) <= 0) {
            for(i = 0; i < numnetworks; i++)
                check_network(&networks[i]);
            set_timeout(-1, CHECK_NETWORKS, 30000, 1);
        }
    }

    if(config_data) {
        if(ipv4_address[0] != 0 && selected_stateful_server >= 0) {
            unsigned short sixteen = htons(16);
            buf[0] = 43;
            buf[1] = 0;
            buf[2] = AHCP_STATEFUL_RELEASE;
            buf[3] = 0;
            memset(buf + 4, 0, 2);
            memcpy(buf + 6, &sixteen, 2);
            memcpy(buf + 8, unique_id, 16);
            rc = build_stateful_data(buf + 24,
                                     ipv4_address[0] == 0 ?
                                     NULL : ipv4_address);
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            memcpy(&sin6.sin6_addr,
                   stateful_servers + 16 * selected_stateful_server,
                   16);
            sin6.sin6_port = htons(port);
            if(debug_level >= 2)
                printf("Sending stateful release.\n");
            rc = ahcp_send(protocol_socket, buf, 24 + rc,
                           (struct sockaddr*)&sin6, sizeof(sin6));
            if(rc < 0) {
                if(errno == ENETUNREACH)
                    set_timeout(-1, CHECK_NETWORKS, 0, 0);
                perror("ahcp_send");
            }
        }
        current_stateful_server = -1;
        rc = unaccept_data(interfaces, dummy);
        if(rc < 0) {
            fprintf(stderr, "Couldn't unconfigure!\n");
            goto fail;
        }
    }
    if(pidfile)
        unlink(pidfile);
    return 0;

 usage:
    fprintf(stderr,
            "Syntax: ahcpd "
            "[-m group] [-p port] [-a authority_file] [-e expires] [-n] [-N]\n"
            "              "
            "[-i file] [-c script] [-s] [-D] [-I pidfile] [-L logfile]\n"
            "              "
#ifndef NO_STATEFUL_SERVER
            "[-S first last dir] "
#endif
            "interface...\n");
    exit(1);

 fail:
    if(pidfile)
        unlink(pidfile);
    exit(1);
}

static void
set_timeout(int net, int which, int msecs, int override)
{
    if(net < 0 && (which == QUERY || which == REPLY)) {
        int i;
        for(i = 0; i < numnetworks; i++)
            set_timeout(i, which, msecs, override);
    } else {
        struct timeval *tv;
        int ms = msecs == 0 ? 0 : msecs / 2 + random() % msecs;
        if(which == QUERY)
            tv = &networks[net].query_time;
        else if(which == REPLY)
            tv = &networks[net].reply_time;
        else if(which == STATEFUL_REQUEST)
            tv = &stateful_request_time;
        else if(which == STATEFUL_EXPIRE)
            tv = &stateful_expire_time;
        else if(which == CHECK_NETWORKS)
            tv = &check_networks_time;
        else
            abort();

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
sigdump(int signo)
{
    dumping = 1;
}

static void
sigchanged(int signo)
{
    changed = 1;
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

    sigemptyset(&ss);
    sa.sa_handler = sigdump;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    sigemptyset(&ss);
    sa.sa_handler = sigchanged;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGUSR2, &sa, NULL);

#ifdef SIGINFO
    sigemptyset(&ss);
    sa.sa_handler = sigdump;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINFO, &sa, NULL);
#endif
}

int
ahcp_socket(int port)
{
    struct sockaddr_in6 sin6;
    int s, rc;
    int saved_errno;
    int one = 1, zero = 0;

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

    rc = setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                    &one, sizeof(one));
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

static int
reopen_logfile()
{
    int lfd, rc;

    if(logfile == NULL)
        return 0;

    lfd = open(logfile, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if(lfd < 0)
        return -1;

    fflush(stdout);
    fflush(stderr);

    rc = dup2(lfd, 1);
    if(rc < 0)
        return -1;

    rc = dup2(lfd, 2);
    if(rc < 0)
        return -1;

    if(lfd > 2)
        close(lfd);

    return 1;
}

static int
daemonise()
{
    int rc;

    fflush(stdout);
    fflush(stderr);

    rc = fork();
    if(rc < 0)
        return -1;

    if(rc > 0)
        exit(0);

    rc = setsid();
    if(rc < 0)
        return -1;

    return 1;
}
