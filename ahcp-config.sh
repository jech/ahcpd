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

    # With MacOS X < 10.7 and a 64bit processor the 'networksetup'
    # command is unable to correctly setup an IPv6 address. For
    # instance:
    #   networksetup -setv6manual Ethernet 1111:2222:3333:4444:5555:6666:7777:8888 128
    # install the following IP:
    #   1111:2222:4444:5555::random_bits
    # The following functions use lower-level tools to fix the incorrect IPv6.

    fix_ipv6_addresses() {

        version=$(sysctl -n kern.osrelease | cut -d . -f 1)

	# Kernel version for MacOS X 10.6.x is 10.x.0
	( [ $version -le 10 ] && [ "$(sysctl -n hw.cpu64bit_capable)" = "1" ] ) || return

        SCUTIL="scutil --prefs /Library/Preferences/SystemConfiguration/preferences.plist"

        service_id=$(scselect 2>&1 | grep " \* " | cut -f 1 | cut -d ' ' -f 3)

        all_intf_ids=$(echo "get /Sets/$service_id/Network/Global/IPv4
d.show" | sudo scutil --prefs /Library/Preferences/SystemConfiguration/preferences.plist | tail -n +3 | grep -v } | cut -d ' ' -f 7)

        get_service_id() {
            for id in ${all_intf_ids}; do
                intf=$(echo "get /NetworkServices/$id/Interface
d.show" | ${SCUTIL} | grep DeviceName | sed 's%  DeviceName : %%')
                if [ "$intf" = "$1" ]; then
                    echo $id
                    return
                fi
            done
            die "Unknown interface"
        }

        id=$(get_service_id $1)
        ${SCUTIL} <<EOF
get /NetworkServices/$id/IPv6
d.add Addresses * $ipv6_address
set /NetworkServices/$id/IPv6
commit
apply
EOF

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
        if [ "$platform" = "Darwin" ]; then
            fix_ipv6_addresses $i
        fi
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
