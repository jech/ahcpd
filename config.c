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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>

#include "ahcpd.h"
#include "config.h"
#include "constants.h"

int data_len = -1;

unsigned char *config_data = NULL;
unsigned char *stateful_servers = NULL;
unsigned int stateful_servers_len = 0;

unsigned char ipv4_address[4];

#define DO_NOTHING 0
#define DO_START 1
#define DO_STOP 2
#define DO_START_IPv4 3
#define DO_STOP_IPv4 4

static int
doit(const unsigned char *data, int len, unsigned char *ipv4,
     int what, char **interfaces);
static char *parse_address_list(const unsigned char *data, int len);

int
data_changed(unsigned char *data, int len)
{
    if(!config_data)
        return 1;

    if(len == data_len && memcmp(config_data, data, len) == 0)
        return 0;

    return 1;
}

int
accept_data(unsigned char *data, int len, char **interfaces, int dummy)
{
    unsigned char *new_data;
    int rc;

    if(len < 4)
        return -1;

    if(!data_changed(data, len))
            return 0;

    unaccept_stateful_data(interfaces);

    rc = doit(data, len, NULL, DO_NOTHING, interfaces);
    if(rc < 0)
        return -1;

    if(config_data && !dummy) {
        rc = doit(config_data, data_len, NULL, DO_STOP, interfaces);
        if(rc < 0) {
            fprintf(stderr, "Ack!  Couldn't unconfigure!\n");
            exit(1);
        }
    }

    new_data = malloc(len);
    if(new_data == NULL)
        return -1;

    memcpy(new_data, data, len);

    if(config_data)
        free(config_data);
    config_data = new_data;
    data_len = len;

    if(!dummy) {
        rc = doit(config_data, data_len, NULL, DO_START, interfaces);
        if(rc < 0) {
            fprintf(stderr, "Ack!  Couldn't configure.\n");
            free(config_data);
            config_data = NULL;
            data_len = -1;
            return -1;
        }
    }

    return 1;
}

int
unaccept_data(char **interfaces, int dummy)
{
    int rc;

    if(!dummy) {
        unaccept_stateful_data(interfaces);
        rc = doit(config_data, data_len, NULL, DO_STOP, interfaces);
        if(rc < 0) {
            fprintf(stderr, "Ack!  Couldn't unconfigure!\n");
            exit(1);
        }
    }
    free(config_data);
    config_data = NULL;
    data_len = -1;
    return 1;
}

static char *script_actions[] =
    { "???", "start", "stop", "start-ipv4", "stop-ipv4" };

static int
doit(const unsigned char *data, int len, unsigned char *ipv4,
     int what, char **interfaces)
{
    int i, opt, olen;
    int mandatory = 0;
    pid_t pid;
    char *prefix = NULL, *nameserver = NULL, *ntp_server = NULL;
    int routing_protocol = 0;
    char *routing_protocol_name = NULL;
    char *static_default_gw = NULL;
    char *olsr_multicast_address = NULL;
    char *babel_multicast_address = NULL;
    int olsr_link_quality = 0;
    int babel_port_number = -1;
    int babel_hello_interval = -1;

    i = 0;

    while(i < len) {
        opt = data[i];
        if(opt == OPT_PAD) {
            mandatory = 0;
            i++;
            continue;
        } else if(opt == OPT_MANDATORY) {
            mandatory = 1;
            i++;
            continue;
        }

        olen = data[i + 1];
        if(olen + 2 + i > len) {
            fprintf(stderr, "Truncated message.\n");
            return -1;
        }
        if(opt == OPT_EXPIRES) {
            struct timeval now;
            unsigned int expires;

            if(olen != 4)
                return -1;
            memcpy(&expires, data + i + 2, 4);
            expires = ntohl(expires);
            gettimeofday(&now, NULL);
            if(now.tv_sec > expires) {
                fprintf(stderr, "Received expired data.\n");
                return -1;
            }
        } else if(opt == OPT_IPv6_PREFIX || opt == OPT_NAME_SERVER ||
                  opt == OPT_NTP_SERVER) {
            char *value;
            if(olen % 16 != 0) {
                fprintf(stderr, "Unexpected length for %s.\n",
                        opt == OPT_IPv6_PREFIX ? "prefix" : "server");
                return -1;
            }
            value = parse_address_list(data + i + 2, olen);
            if(opt == OPT_IPv6_PREFIX)
                prefix = value;
            else if(opt == OPT_NAME_SERVER)
                nameserver = value;
            else if(opt == OPT_NTP_SERVER)
                ntp_server = value;
            else
                abort();
        } else if(opt == OPT_ROUTING_PROTOCOL) {
            int omandatory = 0;
            int j;
            if(olen < 1) {
                fprintf(stderr, "Unexpected size for routing protocol.\n");
                return -1;
            }
            routing_protocol = data[i + 2];
            if(routing_protocol == ROUTING_PROTOCOL_STATIC) {
                routing_protocol_name = "static";
            } else if(routing_protocol == ROUTING_PROTOCOL_OLSR) {
                routing_protocol_name = "OLSR";
            } else if(routing_protocol == ROUTING_PROTOCOL_BABEL) {
                routing_protocol_name = "Babel";
            } else {
                if(routing_protocol != 0) {
                    fprintf(stderr, "Unknown routing protocol %d\n",
                            routing_protocol);
                    routing_protocol = 0;
                }
            }
            j = i + 3;
            while(j < i + 2 + olen) {
                int oopt = data[j], oolen;

                if(oopt == OPT_PAD) {
                    omandatory = 0;
                    j++;
                    continue;
                } else if(oopt == OPT_MANDATORY) {
                    omandatory = 1;
                    j++;
                    continue;
                }

                oolen = data[j + 1];
                if(j + oolen > i + olen) {
                    fprintf(stderr, "Truncated suboption.\n");
                    return -1;
                }
                if((routing_protocol == ROUTING_PROTOCOL_STATIC &&
                    oopt == STATIC_DEFAULT_GATEWAY) ||
                   (routing_protocol == ROUTING_PROTOCOL_OLSR &&
                    oopt == OLSR_MULTICAST_ADDRESS) ||
                   (routing_protocol == ROUTING_PROTOCOL_BABEL &&
                    oopt == BABEL_MULTICAST_ADDRESS)) {
                    char *value;
                    if(oolen % 16 != 0) {
                        fprintf(stderr, "Unexpected length for %s\n",
                                routing_protocol == ROUTING_PROTOCOL_STATIC ?
                                "default gateway" : "multicast address");
                        return -1;
                    }
                    value = parse_address_list(data + j + 2, oolen);
                    if(routing_protocol == ROUTING_PROTOCOL_STATIC)
                        static_default_gw = value;
                    else if(routing_protocol == ROUTING_PROTOCOL_OLSR)
                        olsr_multicast_address = value;
                    else
                        babel_multicast_address = value;
                } else if(routing_protocol == ROUTING_PROTOCOL_OLSR &&
                          oopt == OLSR_LINK_QUALITY) {
                    int value;
                    if(oolen != 1) {
                        fprintf(stderr,
                                "Unexpected length "
                                "for OLSR link quality flag.");
                        return -1;
                    }
                    value = data[j + 2];
                    switch(value) {
                    case 0: olsr_link_quality = 0; break;
                    case 1: olsr_link_quality = 1; break;
                    case 2: olsr_link_quality = 2; break;
                    default:
                        fprintf(stderr,
                                "Unexpected value %d "
                                "for OLSR link quality flag.",
                                olsr_link_quality);
                        return -1;
                    }
                } else if(routing_protocol == ROUTING_PROTOCOL_BABEL &&
                          oopt == BABEL_PORT_NUMBER) {
                    if(oolen != 2) {
                        fprintf(stderr, "Unexpected lengh "
                                "for Babel port number.\n");
                        return -1;
                    }
                    babel_port_number = ((data[j + 2] << 8) | data[j + 3]);
                } else if(routing_protocol == ROUTING_PROTOCOL_BABEL &&
                          oopt == BABEL_HELLO_INTERVAL) {
                    if(oolen != 2) {
                        fprintf(stderr, "Unexpected lengh "
                                "for Babel hello interval.\n");
                        if(omandatory)
                            return -1;
                    }
                    babel_hello_interval = ((data[j + 2] << 8) | data[j + 3]);
                } else {
                    if(omandatory || debug_level >= 1)
                        fprintf(stderr, "Unknown suboption %d\n", oopt);
                    if(omandatory) return -1;
                }
                omandatory = 0;
                j += oolen + 2;
            }
        } else if(opt == OPT_AHCP_STATEFUL_SERVER) {
            if(olen % 16 != 0) {
                fprintf(stderr, "Unexpected length for stateful server.\n");
                return -1;
            }
            if(what == DO_START) {
                stateful_servers = malloc(olen);
                if(stateful_servers == NULL)
                    return -1;
                memcpy(stateful_servers, data + i + 2, olen);
                stateful_servers_len = olen;
            }
        } else {
            if(mandatory || debug_level >= 1)
                fprintf(stderr, "Unsupported option %d\n", opt);
            if(mandatory) return -1;
        }
        mandatory = 0;
        i += olen + 2;
    }

    if(what == DO_NOTHING || config_script[0] == '\0')
        goto success;

    if(what == DO_STOP) {
        if(stateful_servers)
            free(stateful_servers);
        stateful_servers = NULL;
        stateful_servers_len = 0;
    }

    pid = fork();
    if(pid < 0) {
        perror("fork");
        return 0;
    } else if(pid == 0) {
        char buf[200];
        int i;
        snprintf(buf, 50, "%lu", (unsigned long)getppid());
        setenv("AHCP_DAEMON_PID", buf, 1);
        buf[0] = '\0';
        i = 0;
        while(interfaces[i]) {
            if(i > 0)
                strncat(buf, " ", 200);
            strncat(buf, interfaces[i], 200);
            i++;
        }
        setenv("AHCP_INTERFACES", buf, 1);
        snprintf(buf, 50, "%d", debug_level);
        setenv("AHCP_DEBUG_LEVEL", buf, 1);
        if(routing_protocol_name)
            setenv("AHCP_ROUTING_PROTOCOL", routing_protocol_name, 1);
        if(static_default_gw)
            setenv("AHCP_STATIC_DEFAULT_GATEWAY", static_default_gw, 1);
        if(olsr_multicast_address)
            setenv("AHCP_OLSR_MULTICAST_ADDRESS", olsr_multicast_address, 1);
        if(olsr_link_quality) {
            buf[0] = olsr_link_quality + '0';
            buf[1] = '\0';
            setenv("AHCP_OLSR_LINK_QUALITY", buf, 1);
        }
        if(babel_multicast_address)
            setenv("AHCP_BABEL_MULTICAST_ADDRESS",
                   babel_multicast_address, 1);
        if(babel_port_number >= 0) {
            snprintf(buf, 50, "%d", babel_port_number);
            setenv("AHCP_BABEL_PORT_NUMBER", buf, 1);
        }
        if(babel_hello_interval >= 0) {
            snprintf(buf, 50, "%d", babel_hello_interval);
            setenv("AHCP_BABEL_HELLO_INTERVAL", buf, 1);
        }
        if(prefix) {
            setenv("AHCP_PREFIX", prefix, 1);
        }
        if(nameserver) {
            if(!nodns)
                setenv("AHCP_NAMESERVER", nameserver, 1);
        }
        if(ntp_server) {
            setenv("AHCP_NTP_SERVER", ntp_server, 1);
        }
        if(ipv4) {
            char buf[100];
            const char *p;
            p = inet_ntop(AF_INET, ipv4, buf, 100);
            if(p == NULL)
                return -1;
            setenv("AHCP_IPv4_ADDRESS", buf, 1);
        }

        if(noroute)
            setenv("AHCP_DONT_START_ROUTING_PROTOCOL", "true", 1);

        if(debug_level >= 1)
            printf("Running ``%s %s''\n", config_script,
                   script_actions[what]);
        execl(config_script, config_script, script_actions[what], NULL);
        perror("exec failed");
        exit(42);
    } else {
        int status;
    again:
        pid = waitpid(pid, &status, 0);
        if(pid < 0) {
            if(errno == EINTR)
                goto again;
            perror("wait");
            return -1;
        } else if(!WIFEXITED(status)) {
            fprintf(stderr, "Child died violently (%d)\n", status);
            return -1;
        } else if(WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Child returned error status %d\n",
                    WEXITSTATUS(status));
            return -1;
        }
    }

 success:
    if(prefix) free(prefix);
    if(nameserver) free(nameserver);
    if(ntp_server) free(ntp_server);
    if(static_default_gw) free(static_default_gw);
    if(olsr_multicast_address) free(olsr_multicast_address);
    if(babel_multicast_address) free(babel_multicast_address);
    return 1;
}

static char *
ntoa6(const unsigned char *data)
{
    const char *p;
    char buf[100];
    p = inet_ntop(AF_INET6, data, buf, 100);
    if(p == NULL) return NULL;
    return strdup(p);
}

static char *
parse_address_list(const unsigned char *data, int len)
{
    char *result = NULL, *value;
    int i;

    if(len % 16 != 0)
        return NULL;

    i = 0;
    while(i < len) {
        value = ntoa6(data + i);
        if(value == NULL) {
            fprintf(stderr, "Couldn't grok IPv6 address");
            if(result) free(result);
            return NULL;
        }
        if(result == NULL) {
            result = value;
        } else {
            char *old_result = result;
            result = realloc(old_result,
                             strlen(old_result) + 1 + strlen(value) + 1);
            if(result == NULL) {
                free(old_result);
                return NULL;
            }
            strcat(result, " ");
            strcat(result, value);
            free(value);
        }
        i += 16;
    }
    return result;
}

int
parse_stateful_data(unsigned char *data, int len, unsigned char *ipv4_return)
{
    int i, opt, olen, mandatory = 0;
    unsigned char ipv4[4] = {0, 0, 0, 0};

    i = 0;
    while(i < len) {
        opt = data[i];
        if(opt == OPT_PAD) {
            mandatory = 0;
            i++;
            continue;
        } else if(opt == OPT_MANDATORY) {
            mandatory = 1;
            i++;
            continue;
        }

        olen = data[i + 1];
        if(olen + 2 + i > len) {
            fprintf(stderr, "Truncated message.\n");
            return -1;
        }
        if(opt == OPT_IPv4_ADDRESS) {
            if(olen < 4 || olen % 4 != 0)
                return -1;
            memcpy(ipv4, data + i + 2, 4);
        } else {
            if(mandatory) return -1;
        }
        mandatory = 0;
        i += olen + 2;
    }

    memcpy(ipv4_return, ipv4, 4);
    return 1;
}

int
accept_stateful_data(unsigned char *data, int len, unsigned short lease_time,
                     char **interfaces)
{
    const unsigned char z[4] = {0, 0, 0, 0};
    unsigned char ipv4[4] = {0, 0, 0, 0};
    int rc;

    if(!config_data) {
        fprintf(stderr, "Attempted to configure IPv4 while unconfigured.\n");
        return -1;
    }

    rc = parse_stateful_data(data, len, ipv4);
    if(rc < 0)
        return -1;

    if(memcmp(ipv4_address, z, 4) == 0) {
        rc = doit(config_data, data_len, ipv4, DO_START_IPv4, interfaces);
        if(rc < 0)
            return -1;
        memcpy(ipv4_address, ipv4, 4);
        return 1;
    } else if(memcmp(ipv4_address, ipv4, 4) != 0) {
        return -1;
    } else {
        return 0;
    }
}

int
unaccept_stateful_data(char **interfaces)
{
    const unsigned char z[4] = {0, 0, 0, 0};
    int rc;

    if(memcmp(ipv4_address, z, 4) == 0) {
        return 0;
    } else {
        if(!config_data) {
            fprintf(stderr,
                    "Attempted to unconfigure IPv4 while unconfigured.\n");
            return -1;
        }

        rc = doit(config_data, data_len, ipv4_address,
                  DO_STOP_IPv4, interfaces);
        if(rc < 0)
            return -1;
        memcpy(ipv4_address, z, 4);
        return 1;
    }
}

int
build_stateful_data(unsigned char *buf, const unsigned char *ipv4)
{
    const unsigned short zero = htons(0);
    const unsigned short six = htons(6);

    if(ipv4) {
        memcpy(buf, &six, 2);
        buf[2] = OPT_IPv4_ADDRESS;
        buf[3] = 4;
        memcpy(buf + 4, ipv4, 4);
        return 8;
    } else {
        memcpy(buf, &zero, 2);
        return 2;
    }
}
