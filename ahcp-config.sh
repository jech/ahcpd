#!/bin/sh

die() {
    echo $1 >&2
    exit 1
}

findcmd() {
    type "$1" &> /dev/null || \
        die "Couldn't find $1, please install ${2:-it} or fix your path."
}

nl='
'

olsrd_pidfile=/var/run/ahcp_olsrd_pid
babel_pidfile=/var/run/ahcp_babel_pid
usage="Usage: $0 (start|stop)"
debuglevel=${AHCP_DEBUG_LEVEL:-1}
if [ grep -q ' ' "$AHCP_PREFIX" ] ; then
    echo "Warning: multiple prefixes not supported yet."
    prefix="$(echo $AHCP_PREFIX | sed 's/ .*//')"
else
    prefix="${AHCP_PREFIX:--r}"
fi

[ $# -eq 1 ] || die "$usage"

if [ $debuglevel -ge 1 ]; then
   env | grep AHCP
fi

if [ $debuglevel -ge 2 ]; then
   set -x
fi

interfaces="$AHCP_INTERFACES"
if [ -z "$interfaces" ]; then die "No interface set"; fi
first_if="$(echo $interfaces | sed 's/ .*//')"
other_if="$(echo $interfaces | sed 's/[^ ]*//')"

findcmd ip iproute

(ip -6 addr show dev lo | grep -q 'inet6') || \
    die "No IPv6 address on lo, please modprobe ipv6"

if_address() {
    l="$(ip -0 addr show dev "$1" | grep '^ *link/ether ' | head -n 1)"
    mac="$(echo "$l" | sed 's|^ *link/ether \([0-9a-z:]*\) .*$|\1|')"
    ahcp-generate-address $prefix $mac
}

add_address() {
    ip -6 addr add "${2:-$(if_address $1)}$3" dev "$1"
}

add_addresses() {
    for i in $interfaces; do
        add_address $i
    done
}

del_address() {
    ip -6 addr del "${2:-$(if_address $1)}$3" dev "$1"
}

del_addresses() {
    for i in $interfaces; do
        del_address $i
    done
}

nameserver_start() {
    if [ ! -z "$AHCP_NAMESERVER" ]; then
        info=''
        for n in $AHCP_NAMESERVER; do
            info="${info}nameserver $n$nl"
        done
        if [ -x /sbin/resolvconf ]; then
            echo -n "$info" | /sbin/resolvconf -a "$first_if"
        else
            mv /etc/resolv.conf /etc/resolv.conf.orig
            echo -n "$info" > /etc/resolv.conf
        fi
    fi
}

nameserver_stop() {
    if [ ! -z "$AHCP_NAMESERVER" ]; then
        if [ -x /sbin/resolvconf ]; then
            /sbin/resolvconf -d "$first_if"
        else
            mv /etc/resolv.conf.orig /etc/resolv.conf
        fi
    fi
}

start_static() {
    # add the first interface with a /64 prefix, to make the case with
    # no reachable gateway work
    add_address "$first_if" '' /64
    for i in $other_if; do
        add_address "$i"
    done
    for gw in $AHCP_STATIC_DEFAULT_GATEWAY; do
          ip -6 route add ::/0 via "$gw" dev "$first_if";
    done
}

stop_static() {
    for gw in $AHCP_STATIC_DEFAULT_GATEWAY; do
        ip -6 route del ::/0 via "$gw" dev "$interfaces"
    done
    del_address "$first_if" '' /64
    for i in $other_if; do
        del_address "$i"
    done
}

start_olsr() {
    multicast="$AHCP_OLSR_MULTICAST_ADDRESS"

    conf_filename=/var/run/ahcp-olsrd-"$AHCP_DAEMON_PID".conf

    cat > "$conf_filename" <<EOF
DebugLevel $debuglevel

IpVersion 6

AllowNoInt yes
EOF

    for interface in $interfaces; do
        cat >> "$conf_filename" <<EOF
Interface "$interface" {
    Ip6AddrType global
    Ip6MulticastSite $multicast
    Ip6MulticastGlobal $multicast
}
EOF
    done
    if [ "${AHCP_OLSR_LINK_QUALITY:-0}" -ge 1 ]; then
        echo "UseHysteresis no" >> "$conf_filename"
        echo "LinkQualityLevel $AHCP_OLSR_LINK_QUALITY" >> "$conf_filename"
    fi

    for file in "/usr/local/lib/ahcp/ahcp-olsrd-$(uname -n).conf" \
                "/usr/local/lib/ahcp/ahcp-olsrd.conf" ; do
        if [ -r "$file" ]; then
            sed "s/\$AHCP_OLSR_MULTICAST_ADDRESS/$multicast/" < "$file" \
                >> "$conf_filename"
        fi
    done
    add_addresses
    nameserver_start
    olsrd -f "$conf_filename" -nofork &
    echo $! > $olsrd_pidfile
}

stop_olsr() {
    conf_filename=/var/run/ahcp-olsrd-"$AHCP_DAEMON_PID".conf

    kill $(cat "$olsrd_pidfile")
    rm "$olsrd_pidfile"

    del_addresses
    rm "$conf_filename"
    nameserver_stop
}

start_babel() {
    multicast="${AHCP_BABEL_MULTICAST_ADDRESS:+-m $AHCP_BABEL_MULTICAST_ADDRESS}"
    port="${AHCP_BABEL_PORT_NUMBER:+-p $AHCP_BABEL_PORT_NUMBER}"
    hello="${AHCP_BABEL_HELLO_INTERVAL:+-h $AHCP_BABEL_HELLO_INTERVAL}"

    if [ -r /usr/local/lib/ahcp/ahcp-options ] ; then
        options="$(cat /usr/local/lib/ahcp/ahcp-options)"
    fi

    if [ -r /usr/local/lib/ahcp/ahcp-interfaces ] ; then
        more_interfaces="$(cat /usr/local/lib/ahcp/ahcp-interfaces)"
    fi

    # Babel can work with unnumbered links, so only number the first one
    first_addr="$(if_address $first_if)"
    add_address $first_if $first_addr
    nameserver_start

    babel -d $debuglevel $multicast $port $hello $options $first_addr $interfaces $more_interfaces &
    echo $! > $babel_pidfile
}

stop_babel() {
    kill $(cat "$babel_pidfile")
    rm "$babel_pidfile"

    del_address $first_if
    nameserver_stop
}

case $1 in
    start)
        case ${AHCP_ROUTING_PROTOCOL:-static} in
            static) start_static;;
            OLSR) start_olsr;;
            [Bb]abel) start_babel;;
            *) die "Unknown routing protocol $AHCP_ROUTING_PROTOCOL";;
        esac;;
    stop)
        case ${AHCP_ROUTING_PROTOCOL:-static} in
            static) stop_static;;
            OLSR) stop_olsr;;
            [Bb]abel) stop_babel;;
            *) die "Unknown routing protocol $AHCP_ROUTING_PROTOCOL";;
        esac;;
esac
