/*
Copyright (c) 2006-2008 by Juliusz Chroboczek

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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#if defined(__APPLE__)
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#define bswap_64 OSSwapInt64
#else
#include <endian.h>
#include <byteswap.h>
#endif
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "constants.h"

#define MAXSIZE 2048

struct list;

unsigned int expires_delta = 4 * 30 * 24 * 60 * 60;
struct list *prefixes = NULL;
unsigned char routing_protocol = ROUTING_PROTOCOL_BABEL;

struct list *name_servers = NULL;
struct list *ntp_servers = NULL;
struct list *stateful_servers = NULL;
struct list *default_gateways = NULL;

unsigned char olsr_multicast_address[16] =
    { 0xff, 0x04, 0, 0, 0, 0, 0, 0,
      0xcc, 0xa6, 0xc0, 0xf9, 0xe1, 0x82, 0x53, 0x59 };
unsigned char babel_multicast_address[16] =
    { 0xff, 0x02, 0, 0, 0, 0, 0, 0,
      0xcc, 0xa6, 0xc0, 0xf9, 0xe1, 0x82, 0x53, 0x73 };
unsigned int babel_port_number = 8475;
unsigned char link_quality = 0;

struct list {
    char *head;
    struct list *tail;
};

static struct list *
cons(char *head, struct list *tail)
{
    struct list *cell;

    cell = malloc(sizeof(struct list));
    if(cell == NULL) abort();

    cell->head = head;
    cell->tail = tail;
    return cell;
}

static struct list *
reverse(struct list *list, struct list *result)
{
    struct list *tail;

    if(list == NULL)
        return result;

    tail = list->tail;
    list->tail = result;
    return reverse(tail, list);
}

static int
length(struct list *list)
{
    if(list == NULL)
        return 0;

    return 1 + length(list->tail);
}

int
main(int argc, char **argv)
{
    unsigned char buf[MAXSIZE];
    union {
        unsigned short s;
        char c[2];
    } twobytes;
    union {
        unsigned int i;
        char c[4];
    } fourbytes;
    int i;
    char *usage =
        "ahcp-generate "
        "-p prefix [-P protocol] [-g gw] [-n name-server] [-N ntp-server]\n"
        "              "
        "[-s stateful-server] [-e seconds] > ahcp.dat";
    int rc;
    struct list *l;

    while(1) {
        int c;
        c = getopt(argc, argv, "p:n:N:g:P:s:e:");
        if(c < 0) break;
        switch(c) {
        case 'p': prefixes = cons(optarg, prefixes); break;
        case 'n': name_servers = cons(optarg, name_servers); break;
        case 'N': ntp_servers = cons(optarg, ntp_servers); break;
        case 'g': default_gateways = cons(optarg, default_gateways); break;
        case 'P':
            if(strcasecmp(optarg, "static") == 0)
                routing_protocol = ROUTING_PROTOCOL_STATIC;
            else if(strcasecmp(optarg, "olsr") == 0)
                routing_protocol = ROUTING_PROTOCOL_OLSR;
            else if(strcasecmp(optarg, "babel") == 0)
                routing_protocol = ROUTING_PROTOCOL_BABEL;
            else {
                fprintf(stderr, "Unknown routing protocol %s.\n", optarg);
                exit(1);
            }
            break;
        case 's': stateful_servers = cons(optarg, stateful_servers); break;
        case 'e': expires_delta = atoi(optarg); break;
        default: fprintf(stderr, "%s\n", usage); exit(1); break;
        }
    }

    if(prefixes == NULL) {
        fprintf(stderr, "%s\n", usage);
        exit(1);
    }

    if(argc != optind) {
        fprintf(stderr, "%s\n", usage); exit(1);
    }

    prefixes = reverse(prefixes, NULL);
    name_servers = reverse(name_servers, NULL);
    ntp_servers = reverse(ntp_servers, NULL);
    stateful_servers = reverse(stateful_servers, NULL);
    default_gateways = reverse(default_gateways, NULL);

    i = 0;

#define EMIT(a, n) if(i > MAXSIZE - n) goto fail; memcpy(buf + i, a, n); i += n;

#define EMIT1(b) if(i > MAXSIZE - 1) goto fail; buf[i++] = b;

#define EMIT2(ss) do { \
    if(i > MAXSIZE - 2) goto fail; \
    twobytes.s = htons(ss); \
    buf[i++] = twobytes.c[0]; \
    buf[i++] = twobytes.c[1]; } while(0)

#define EMIT4(n) do {               \
    if(i > MAXSIZE - 4) goto fail; \
    fourbytes.i = htonl(n); \
    buf[i++] = fourbytes.c[0]; \
    buf[i++] = fourbytes.c[1]; \
    buf[i++] = fourbytes.c[2]; \
    buf[i++] = fourbytes.c[3]; } while (0)

#define EMIT8(l) do { \
    if(i > MAXSIZE - 8) goto fail; \
    eightbytes.i = htonll(l); \
    buf[i++] = eightbytes.c[0]; \
    buf[i++] = eightbytes.c[1]; \
    buf[i++] = eightbytes.c[2]; \
    buf[i++] = eightbytes.c[3]; \
    buf[i++] = eightbytes.c[4]; \
    buf[i++] = eightbytes.c[5]; \
    buf[i++] = eightbytes.c[6]; \
    buf[i++] = eightbytes.c[7]; } while(0)

    if(expires_delta > 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_EXPIRES);
        EMIT1(4);
        EMIT4(now.tv_sec + expires_delta);
    }

    if(prefixes) {
        unsigned char p[16];
        EMIT1(OPT_IPv6_PREFIX);
        EMIT1(16 * length(prefixes));
        l = prefixes;
        while(l) {
            rc = inet_pton(AF_INET6, l->head, p);
            if(rc < 0) {
                fprintf(stderr, "Couldn't parse prefix.\n");
                exit(1);
            }
            EMIT(p, 16);
            l = l->tail;
        }
    }

    if(routing_protocol == ROUTING_PROTOCOL_STATIC) {
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_ROUTING_PROTOCOL);
        EMIT1(default_gateways ? 3 + 16 * length(default_gateways) : 1);
        EMIT1(ROUTING_PROTOCOL_STATIC);
        if(default_gateways) {
            unsigned char g[16];
            EMIT1(STATIC_DEFAULT_GATEWAY);
            EMIT1(16 * length(default_gateways));
            l = default_gateways;
            while(l) {
                rc = inet_pton(AF_INET6, l->head, g);
                if(rc < 0) {
                    fprintf(stderr, "Couldn't parse default gateway.\n");
                    exit(1);
                }
                EMIT(g, 16);
                l = l->tail;
            }
        }
    } else if(routing_protocol == ROUTING_PROTOCOL_OLSR) {
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_ROUTING_PROTOCOL);
        EMIT1(link_quality ? 23 : 19);
        EMIT1(ROUTING_PROTOCOL_OLSR);
        EMIT1(OLSR_MULTICAST_ADDRESS);
        EMIT1(16);
        EMIT(olsr_multicast_address, 16);
        if(link_quality) {
            EMIT1(OPT_MANDATORY);
            EMIT1(OLSR_LINK_QUALITY);
            EMIT1(1);
            EMIT1(link_quality);
        }
    } else if(routing_protocol == ROUTING_PROTOCOL_BABEL) {
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_ROUTING_PROTOCOL);
        EMIT1(23);
        EMIT1(ROUTING_PROTOCOL_BABEL)
        EMIT1(BABEL_MULTICAST_ADDRESS);
        EMIT1(16);
        EMIT(babel_multicast_address, 16);
        EMIT1(BABEL_PORT_NUMBER);
        EMIT1(2);
        EMIT2(babel_port_number);
    }

    if(name_servers) {
        unsigned char n[16];
        EMIT1(OPT_NAME_SERVER);
        EMIT1(16 * length(name_servers));
        l = name_servers;
        while(l) {
            rc = inet_pton(AF_INET6, l->head, n);
            if(rc < 0) {
                fprintf(stderr, "Couldn't parse name server.\n");
                exit(1);
            }
            EMIT(n, 16);
            l = l->tail;
        }
    }

    if(ntp_servers) {
        unsigned char n[16];
        EMIT1(OPT_NTP_SERVER);
        EMIT1(16 * length(ntp_servers));
        l = ntp_servers;
        while(l) {
            rc = inet_pton(AF_INET6, l->head, n);
            if(rc < 0) {
                fprintf(stderr, "Couldn't parse NTP server.\n");
                exit(1);
            }
            EMIT(n, 16);
            l = l->tail;
        }
    }

    if(stateful_servers) {
        unsigned char n[16];
        EMIT1(OPT_AHCP_STATEFUL_SERVER);
        EMIT1(16 * length(stateful_servers));
        l = stateful_servers;
        while(l) {
            rc = inet_pton(AF_INET6, l->head, n);
            if(rc < 0) {
                fprintf(stderr, "Couldn't parse stateful server.\n");
                exit(1);
            }
            EMIT(n, 16);
            l = l->tail;
        }
    }

    write(1, buf, i);
    return 0;

 fail:
    fprintf(stderr, "Buffer overflow");
    exit(1);
}
