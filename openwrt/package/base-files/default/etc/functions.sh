#!/bin/ash

alias debug=${DEBUG:-:}

# valid interface?
if_valid () (
  ifconfig "$1" >&- 2>&- ||
  [ "${1%%[0-9]}" = "br" ] ||
  {
    [ "${1%%[0-9]}" = "vlan" ] && ( 
      i=${1#vlan}
      hwname=$(nvram get vlan${i}hwname)
      hwaddr=$(nvram get ${hwname}macaddr)
      [ -z "$hwaddr" ] && return 1

      vif=$(ifconfig -a | awk '/^eth.*'$hwaddr'/ {print $1; exit}' IGNORECASE=1)
      debug "# vlan$i => $vif"

      $DEBUG ifconfig $vif up
      $DEBUG vconfig add $vif $i 2>&-
    )
  } ||
  { debug "# missing interface '$1' ignored"; false; }
)

do_ifup() {
	if_proto=$(nvram get ${2}_proto)
	if=$(nvram get ${2}_ifname)
	[ "${if%%[0-9]}" = "ppp" ] && if=$(nvram get ${if_proto}_ifname)
	
	pidfile=/var/run/${if}.pid
	[ -f $pidfile ] && $DEBUG kill $(cat $pidfile)

	case "$1" in
	static)
		ip=$(nvram get ${2}_ipaddr)
		netmask=$(nvram get ${2}_netmask)
		gateway=$(nvram get ${2}_gateway)
		mtu=$(nvram get ${2}_mtu)

		$DEBUG ifconfig $if $ip ${netmask:+netmask $netmask} ${mtu:+mtu $(($mtu))} broadcast + up
		${gateway:+$DEBUG route add default gw $gateway}

		[ -f /etc/resolv.conf ] || {
			debug "# --- creating /etc/resolv.conf ---"
			for dns in $(nvram get ${2}_dns); do
				echo "nameserver $dns" >> /etc/resolv.conf
			done
		}
		
		env -i ACTION="ifup" INTERFACE="${2}" PROTO=static /sbin/hotplug "iface" &
	;;
	dhcp)
		DHCP_IP=$(nvram get ${2}_ipaddr)
		DHCP_NETMASK=$(nvram get ${2}_netmask)
		mtu=$(nvram get ${2}_mtu)
		$DEBUG ifconfig $if $ip ${netmask:+netmask $netmask} ${mtu:+mtu $(($mtu))} broadcast + up

		DHCP_ARGS="-i $if ${DHCP_IP:+-r $DHCP_IP} -b -p $pidfile"
		DHCP_HOSTNAME=$(nvram get ${2}_hostname)
		DHCP_HOSTNAME=${DHCP_HOSTNAME%%.*}
		[ -z $DHCP_HOSTNAME ] || DHCP_ARGS="$DHCP_ARGS -H $DHCP_HOSTNAME"
		[ "$if_proto" = "pptp" ] && DHCP_ARGS="$DHCP_ARGS -n -q" || DHCP_ARGS="$DHCP_ARGS -R &"
		oldpid=$(cat $pidfile)
		${DEBUG:-eval} "udhcpc $DHCP_ARGS"
		pidof udhcpc | grep "$oldpid" >&- 2>&- && {
			sleep 1
			kill -9 $oldpid
		}
		# hotplug events are handled by /usr/share/udhcpc/default.script
	;;
	none|"")
	;;
	*)
		[ -x "/sbin/ifup.$1" ] && { $DEBUG /sbin/ifup.$1 ${2}; exit; }
		echo "### ifup ${2}: ignored ${2}_proto=\"$1\" (not supported)"
	;;
	esac
}
