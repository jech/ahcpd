#!/bin/sh

die() {
    echo "$@" >&2
    exit 1
}

findcmd() {
    type "$1" > /dev/null 2>&1 || \
        die "Couldn't find $1, please install ${2:-it} or fix your path."
}

first() {
    echo "$1"
}

nl='
'

usage="Usage: $0 (start|stop)"
debuglevel=${AHCP_DEBUG_LEVEL:-1}

if [ $debuglevel -ge 2 ]; then
    env | grep ^AHCP
fi

[ $debuglevel -ge 3 ] && set -x

interfaces="$AHCP_INTERFACES"
if [ -z "$interfaces" ]; then die "No interface set"; fi
first_if=$(first $interfaces)

ipv4_address="$AHCP_IPv4_ADDRESS"
ipv6_address="$AHCP_IPv6_ADDRESS"

platform=`uname`

if [ "$platform" = "Linux" ]; then

    findcmd ip iproute

    (ip -6 addr show dev lo | grep -q 'inet6') || \
	die "No IPv6 address on lo, please modprobe ipv6"

    add_ipv6_address() {
	ip -6 addr add "$2/$3" dev "$1"
    }

    del_ipv6_address() {
	ip -6 addr del "$2/$3" dev "$1"
    }

    add_ipv4_address() {
	ip addr add "$2/$3" dev "$1"
    }

    del_ipv4_address() {
	ip addr del "$2/$3" dev "$1"
    }

elif [ "$platform" = "Darwin" ]; then

    findcmd networksetup

    getServiceName() {
        networksetup -listnetworkserviceorder | \
          grep -B1 "Device: $1" | \
          head -n 1 | \
          sed 's/([0-9]*) //'
    }

    add_ipv6_address() {
        networksetup -setv6manual $(getServiceName "$1") "$2" "$3"
    }

    del_ipv6_address() {
        networksetup -setv6automatic $(getServiceName "$1")
    }

    add_ipv4_address() {
        case "$3" in
          32)
            mask="255.255.255.255"
            ;;
          *)
            die "add_ipv4_address: unexpected prefix length: $3."
            ;;
          esac
          networksetup -setmanual $(getServiceName "$1") "$2" "$mask"
          # Remove the dummy default route if it was wrongly introduced
          # by OS X before babel
          sleep 2;
          if netstat -rn -f inet | grep default | grep -v UG2c > /dev/null; then
            route delete default >/dev/null
          fi
    }

    del_ipv4_address() {
        networksetup -setv4off $(getServiceName "$1")
    }

else

    add_ipv6_address() {
	ifconfig "$1" inet6 "$2/$3"
    }

    del_ipv6_address() {
	ifconfig "$1" inet6 "$2/$3" delete
    }

    add_ipv4_address() {
	ifconfig "$1" inet "$2/$3"
    }

    del_ipv4_address() {
	ifconfig "$1" inet "$2/$3" delete
    }

fi

add_addresses() {
    for i in $interfaces; do
        for a in $ipv6_address; do
            add_ipv6_address $i $a 128
        done
        for a in $ipv4_address; do
            add_ipv4_address $i $a 32
        done
    done
}

del_addresses() {
    for i in $interfaces; do
        for a in $ipv6_address; do
            del_ipv6_address $i $a 128
        done
        for a in $ipv4_address; do
            del_ipv4_address $i $a 32
        done
    done
}

if [ "$platform" = "Darwin" ]; then

    nameserver_start() {
        if [ ! -z "$AHCP_NAMESERVER" ]; then
            networksetup -setdnsservers $(getServiceName "$first_if") $AHCP_NAMESERVER
        fi
    }

    nameserver_stop() {
        if [ ! -z "$AHCP_NAMESERVER" ]; then
            networksetup -setdnsservers $(getServiceName "$first_if") Empty
        fi
    }

else

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

fi

case $1 in
    start)
        add_addresses
        nameserver_start
        ;;
    stop)
        nameserver_stop
        del_addresses
        ;;
    *)
        die "$usage"
        ;;
esac

[ -x /etc/ahcp/ahcp-local.sh ] && /etc/ahcp/ahcp-local.sh $1

exit 0
