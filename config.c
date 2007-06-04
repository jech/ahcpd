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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
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

static int
parse_data(const unsigned char *data, int len, int start, char **interfaces);
static char *parse_address_list(const unsigned char *data, int len);

int
accept_data(unsigned char *data, int len, char **interfaces, int dummy)
{
    unsigned char *new_data;
    int rc;

    if(len < 4)
        return -1;

    if(config_data) {
        if(len == data_len && memcmp(config_data, data, len) == 0)
            return 0;
    }

    rc = parse_data(data, len, -1, interfaces);
    if(rc < 0)
        return -1;

    if(config_data && !dummy) {
        rc = parse_data(config_data, data_len, 0, interfaces);
        if(rc < 0) {
            fprintf(stderr, "Ack!  Couldn't unconfigure!\n");
            exit(1);
        }
    }

    new_data = malloc(len);
    if(new_data == NULL)
        return -1;

    memcpy(new_data, data, len);

    free(config_data);
    config_data = new_data;
    data_len = len;

    if(!dummy) {
        rc = parse_data(config_data, data_len, 1, interfaces);
        if(rc < 0) {
            fprintf(stderr, "Ack!  Couldn't configure.\n");
            return -2;
        }
    }

    return 1;
}

int
unaccept_data(char **interfaces, int dummy)
{
    int rc;

    if(!dummy) {
        rc = parse_data(config_data, data_len, 0, interfaces);
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

/* Parse a chunk of configuration data.  When parsing is successful,
   configure if start is 1, unconfigure if start is 0, do nothing if
   start is -1. */

static int
parse_data(const unsigned char *data, int len, int start, char **interfaces)
{
    int i, opt, olen;
    int mandatory = 0;
    pid_t pid;
    char *prefix = NULL, *nameserver = NULL;
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
        } else if(opt == OPT_IPv6_PREFIX || opt == OPT_NAME_SERVER) {
            char *value;
            if(olen % 16 != 0) {
                fprintf(stderr, "Unexpected length for %s.\n",
                        opt == OPT_IPv6_PREFIX ? "prefix" : "name server");
                return -1;
            }
            value = parse_address_list(data + i + 2, olen);
            if(opt == OPT_IPv6_PREFIX)
                prefix = value;
            else
                nameserver = value;
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
                    fprintf(stderr, "Unknown suboption %d\n", oopt);
                    if(omandatory)
                        return -1;
                }
                omandatory = 0;
                j += oolen + 2;
            }
        } else {
            fprintf(stderr, "Unsupported option %d\n", opt);
            if(mandatory) return -1;
        }
        mandatory = 0;
        i += olen + 2;
    }

    if(start < 0 || config_script[0] == '\0')
        return 1;

    pid = fork();
    if(pid < 0) {
        perror("fork");
        return 0;
    } else if(pid == 0) {
        char buf[200];
        char *action;
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
        snprintf(buf, 50, "%d", 0); /* XXX for now */
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
            free(prefix);
        }
        if(nameserver) {
            if(!nodns)
                setenv("AHCP_NAMESERVER", nameserver, 1);
            free(nameserver);
        }
        switch(start) {
        case 0: action = "stop"; break;
        case 1: action = "start"; break;
        default: abort();
        }
        fprintf(stderr, "%s\n", config_script);
        execl(config_script, config_script, action, NULL);
        perror("exec failed");
        exit(42);
    } else {
        int status;
        pid = waitpid(pid, &status, 0);
        if(pid < 0)
            perror("wait");
        if(!WIFEXITED(status)) {
            fprintf(stderr, "Child died violently (%d)\n", status);
            return -1;
        }
        if(WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Child returned error status %d\n",
                    WEXITSTATUS(status));
            return -1;
        }
    }
    return 1;
}

static char *
ntoa6(const unsigned char *data)
{
    const char *p;
    char buf1[17], buf2[100];
    memcpy(buf1, data, 16);
    buf1[16] = '\0';
    p = inet_ntop(AF_INET6, buf1, buf2, 100);
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
            if(result == NULL) return NULL;
            strcat(result, " ");
            strcat(result, value);
            free(old_result);
        }
        i += 16;
    }
    return result;
}
