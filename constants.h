/*
Copyright (c) 2006-2007 by Juliusz Chroboczek

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

#define OPT_PAD 0
#define OPT_MANDATORY 1
#define OPT_EXPIRES 2
#define OPT_IPv6_PREFIX 3
#define OPT_ROUTING_PROTOCOL 4
#define OPT_NAME_SERVER 5
#define OPT_NTP_SERVER 6

#define ROUTING_PROTOCOL_STATIC 0
#define ROUTING_PROTOCOL_OLSR 1
#define ROUTING_PROTOCOL_BABEL 2

/* STATIC options */
#define STATIC_DEFAULT_GATEWAY 2

/* OLSR options */
#define OLSR_MULTICAST_ADDRESS 2
#define OLSR_HELLO_INTERVAL 3
#define OLSR_HELLO_VALIDITY 4
#define OLSR_TC_INTERVAL 5
#define OLSR_TC_VALIDITY 6
#define OLSR_MID_INTERVAL 7
#define OLSR_MID_VALIDITY 8
#define OLSR_HNA_INTERVAL 9
#define OLSR_HNA_VALIDITY 10
#define OLSR_LINK_QUALITY 11

/* Babel options */

#define BABEL_MULTICAST_ADDRESS 2
#define BABEL_PORT_NUMBER 3
#define BABEL_HELLO_INTERVAL 4
