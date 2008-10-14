#!/bin/sh 
# Copyright (C) 2008 John Crispin <blogic@openwrt.org>

. /etc/functions.sh

IPTABLES="echo iptables"
IPTABLES=iptables

config_clear
include /lib/network
scan_interfaces

CONFIG_APPEND=1
config_load firewall

config fw_zones
ZONE_LIST=$CONFIG_SECTION

CUSTOM_CHAINS=1
DEF_INPUT=DROP
DEF_OUTPUT=DROP
DEF_FORWARD=DROP

load_policy() {
	config_get input $1 input
	config_get output $1 output
	config_get forward $1 forward

	DEF_INPUT="${input:-$DEF_INPUT}"
	DEF_OUTPUT="${output:-$DEF_OUTPUT}"
	DEF_FORWARD="${forward:-$DEF_FORWARD}"
}

create_zone() {
	local exists
	
	[ "$1" == "loopback" ] && return

	config_get exists $ZONE_LIST $1
	[ -n "$exists" ] && return
	config_set $ZONE_LIST $1 1 

	$IPTABLES -N zone_$1
	$IPTABLES -N zone_$1_ACCEPT
	$IPTABLES -N zone_$1_DROP
	$IPTABLES -N zone_$1_REJECT
	$IPTABLES -N zone_$1_forward
	$IPTABLES -A zone_$1_forward -j zone_$1_$5
	$IPTABLES -A zone_$1 -j zone_$1_$3
	$IPTABLES -A output -j zone_$1_$4
	$IPTABLES -N zone_$1_nat -t nat
	$IPTABLES -N zone_$1_prerouting -t nat
	[ "$6" == "1" ] && $IPTABLES -t nat -A POSTROUTING -j zone_$1_nat
}

addif() {
	local dev
	config_get dev core $2
	[ -n "$dev" -a "$dev" != "$1" ] && delif "$dev" "$2"
	[ -n "$dev" -a "$dev" == "$1" ] && return
	logger "adding $1 to firewall zone $2"
	$IPTABLES -A input -i $1 -j zone_$2
	$IPTABLES -I zone_$2_ACCEPT 1 -o $1 -j ACCEPT
	$IPTABLES -I zone_$2_DROP 1 -o $1 -j DROP
	$IPTABLES -I zone_$2_REJECT 1 -o $1 -j reject
	$IPTABLES -I zone_$2_ACCEPT 1 -i $1 -j ACCEPT
	$IPTABLES -I zone_$2_DROP 1 -i $1 -j DROP
	$IPTABLES -I zone_$2_REJECT 1 -i $1 -j reject
	$IPTABLES -I zone_$2_nat 1 -t nat -o $1 -j MASQUERADE 
	$IPTABLES -I PREROUTING 1 -t nat -i $1 -j zone_$2_prerouting 
	$IPTABLES -A forward -i $1 -j zone_$2_forward
	uci_set_state firewall core "$2" "$1"
}

delif() {
	logger "removing $1 from firewall zone $2"
	$IPTABLES -D input -i $1 -j zone_$2
	$IPTABLES -D zone_$2_ACCEPT -o $1 -j ACCEPT
	$IPTABLES -D zone_$2_DROP -o $1 -j DROP
	$IPTABLES -D zone_$2_REJECT -o $1 -j reject
	$IPTABLES -D zone_$2_ACCEPT -i $1 -j ACCEPT
	$IPTABLES -D zone_$2_DROP -i $1 -j DROP
	$IPTABLES -D zone_$2_REJECT -i $1 -j reject
	$IPTABLES -D zone_$2_nat -t nat -o $1 -j MASQUERADE 
	$IPTABLES -D PREROUTING -t nat -i $1 -j zone_$2_prerouting 
	$IPTABLES -D forward -i $1 -j zone_$2_forward
	uci_revert_state firewall core "$2"
}

load_synflood() {
	local rate=${1:-25}
	local burst=${2:-50}
	echo "Loading synflood protection"
	$IPTABLES -N syn_flood
	$IPTABLES -A syn_flood -p tcp --syn -m limit --limit $rate/second --limit-burst $burst -j RETURN
	$IPTABLES -A syn_flood -j DROP
	$IPTABLES -A INPUT -p tcp --syn -j syn_flood
}

fw_set_chain_policy() {
	local chain=$1
	local target=$2
	[ "$target" == "REJECT" ] && {
		$IPTABLES -A $chain -j reject
		target=DROP
	}
	$IPTABLES -P $chain $target
}

fw_defaults() {
	[ -n "$DEFAULTS_APPLIED" ] && {
		echo "Error: multiple defaults sections detected"
		return;
	}
	DEFAULTS_APPLIED=1

	load_policy "$1"

	echo 1 > /proc/sys/net/ipv4/tcp_syncookies
	for f in /proc/sys/net/ipv4/conf/*/accept_redirects 
	do
		echo 0 > $f
	done
	for f in /proc/sys/net/ipv4/conf/*/accept_source_route 
	do
		echo 0 > $f
	done                                                                   
	
	uci_revert_state firewall core
	uci_set_state firewall core "" firewall_state 

	$IPTABLES -P INPUT DROP
	$IPTABLES -P OUTPUT DROP
	$IPTABLES -P FORWARD DROP

	$IPTABLES -F
	$IPTABLES -t mangle -F
	$IPTABLES -t nat -F
	$IPTABLES -t mangle -X
	$IPTABLES -t nat -X
	$IPTABLES -X
	
	$IPTABLES -A INPUT -m state --state INVALID -j DROP
	$IPTABLES -A INPUT -m state --state RELATED,ESTABLISHED -j ACCEPT
		
	$IPTABLES -A OUTPUT -m state --state INVALID -j DROP
	$IPTABLES -A OUTPUT -m state --state RELATED,ESTABLISHED -j ACCEPT
	
	$IPTABLES -A FORWARD -m state --state INVALID -j DROP
	$IPTABLES -A FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu
	$IPTABLES -A FORWARD -m state --state RELATED,ESTABLISHED -j ACCEPT
	
	$IPTABLES -A INPUT -i lo -j ACCEPT
	$IPTABLES -A OUTPUT -o lo -j ACCEPT

	config_get syn_flood $1 syn_flood
	config_get syn_rate $1 syn_rate
	config_get syn_burst $1 syn_burst
	[ "$syn_flood" == "1" ] && load_synflood $syn_rate $syn_burst
	
	echo "Adding custom chains"
	fw_custom_chains

	$IPTABLES -N input
	$IPTABLES -N output
	$IPTABLES -N forward

	$IPTABLES -A INPUT -j input
	$IPTABLES -A OUTPUT -j output
	$IPTABLES -A FORWARD -j forward

	$IPTABLES -N reject
	$IPTABLES -A reject -p tcp -j REJECT --reject-with tcp-reset
	$IPTABLES -A reject -j REJECT --reject-with icmp-port-unreachable

	fw_set_chain_policy INPUT "$DEF_INPUT"
	fw_set_chain_policy OUTPUT "$DEF_OUTPUT"
	fw_set_chain_policy FORWARD "$DEF_FORWARD"
}

fw_zone() {
	local name
	local network
	local masq

	config_get name $1 name
	config_get network $1 network
	config_get masq $1 masq
	load_policy $1

	[ -z "$network" ] && network=$name
	create_zone "$name" "$network" "$input" "$output" "$forward" "$masq"
	fw_custom_chains_zone "$name"
}

fw_rule() {
	local src 
	local src_ip
	local src_mac
	local src_port
	local src_mac
	local dest
	local dest_ip
	local dest_port
	local proto
	local target
	local ruleset

	config_get src $1 src
	config_get src_ip $1 src_ip
	config_get src_mac $1 src_mac
	config_get src_port $1 src_port
	config_get dest $1 dest
	config_get dest_ip $1 dest_ip
	config_get dest_port $1 dest_port
	config_get proto $1 proto
	config_get target $1 target
	config_get ruleset $1 ruleset
	
	ZONE=input
	TARGET=$target
	[ -z "$target" ] && target=DROP
	[ -n "$src" -a -z "$dest" ] && ZONE=zone_$src
	[ -n "$src" -a -n "$dest" ] && ZONE=zone_${src}_forward
	[ -n "$dest" ] && TARGET=zone_${dest}_$target
	add_rule() {
		$IPTABLES -I $ZONE 1 \
			${proto:+-p $proto} \
			${src_ip:+-s $src_ip} \
			${src_port:+--sport $src_port} \
			${src_mac:+-m mac --mac-source $src_mac} \
			${dest_ip:+-d $dest_ip} \
			${dest_port:+--dport $dest_port} \
			-j $TARGET 
	}
	[ "$proto" == "tcpudp" -o -z "$proto" ] && {
		proto=tcp
		add_rule
		proto=udp
		add_rule
		return
	}
	add_rule
}

fw_forwarding() {
	local src
	local dest
	local masq

	config_get src $1 src
	config_get dest $1 dest
	[ -n "$src" ] && z_src=zone_${src}_forward || z_src=forward
	[ -n "$dest" ] && z_dest=zone_${dest}_ACCEPT || z_dest=ACCEPT
	$IPTABLES -I $z_src 1 -j $z_dest
}

fw_redirect() {
	local src
	local src_ip
	local src_port
	local src_dport
	local src_mac
	local dest_ip
	local dest_port dest_port2
	local proto
	
	config_get src $1 src
	config_get src_ip $1 src_ip
	config_get src_port $1 src_port
	config_get src_dport $1 src_dport
	config_get src_mac $1 src_mac
	config_get dest_ip $1 dest_ip
	config_get dest_port $1 dest_port
	config_get proto $1 proto
	[ -z "$src" -o -z "$dest_ip" ] && { \
		echo "redirect needs src and dest_ip"; return ; }
	
	src_port_first=${src_port%-*}
	src_port_last=${src_port#*-}
	[ "$src_port_first" -ne "$src_port_last" ] && { \
		src_port="$src_port_first:$src_port_last"; }

	src_dport_first=${src_dport%-*}
	src_dport_last=${src_dport#*-}
	[ "$src_dport_first" -ne "$src_dport_last" ] && { \
		src_dport="$src_dport_first:$src_dport_last"; }

	dest_port2=$dest_port
	dest_port_first=${dest_port2%-*}
	dest_port_last=${dest_port2#*-}
	[ "$dest_port_first" -ne "$dest_port_last" ] && { \
		dest_port2="$dest_port_first:$dest_port_last"; }

	add_rule() {
		$IPTABLES -A zone_${src}_prerouting -t nat \
			${proto:+-p $proto} \
			${src_ip:+-s $src_ip} \
			${src_port:+--sport $src_port} \
			${src_dport:+--dport $src_dport} \
			${src_mac:+-m mac --mac-source $src_mac} \
			-j DNAT --to-destination $dest_ip${dest_port:+:$dest_port}

		$IPTABLES -I zone_${src}_forward 1 \
			${proto:+-p $proto} \
			-d $dest_ip \
			${src_ip:+-s $src_ip} \
			${src_port:+--sport $src_port} \
			${dest_port2:+--dport $dest_port2} \
			${src_mac:+-m mac --mac-source $src_mac} \
			-j ACCEPT 
	}
	[ "$proto" == "tcpudp" -o -z "$proto" ] && {
		proto=tcp
		add_rule
		proto=udp
		add_rule
		return
	}
	add_rule
}

fw_include() {
	local path
	config_get path $1 path
	[ -e $path ] && . $path
}

fw_addif() {
	local up
	local ifname
	config_get up $1 up
	config_get ifname $1 ifname
	[ -n "$up" ] || return 0
	(ACTION="ifup" INTERFACE="$1" . /etc/hotplug.d/iface/20-firewall)
}

fw_custom_chains() {
	[ -n "$CUSTOM_CHAINS" ] || return 0
	$IPTABLES -N input_rule
	$IPTABLES -N output_rule
	$IPTABLES -N forwarding_rule
	$IPTABLES -N prerouting_rule -t nat
	$IPTABLES -N postrouting_rule -t nat
			
	$IPTABLES -A INPUT -j input_rule
	$IPTABLES -A OUTPUT -j output_rule
	$IPTABLES -A FORWARD -j forwarding_rule
	$IPTABLES -A PREROUTING -t nat -j prerouting_rule
	$IPTABLES -A POSTROUTING -t nat -j postrouting_rule
}

fw_custom_chains_zone() {
	local zone="$1"

	[ -n "$CUSTOM_CHAINS" ] || return 0
	$IPTABLES -N input_${zone}
	$IPTABLES -N forwarding_${zone}
	$IPTABLES -N prerouting_${zone} -t nat
	$IPTABLES -I zone_${zone} 1 -j input_${zone}
	$IPTABLES -I zone_${zone}_forward 1 -j forwarding_${zone}
	$IPTABLES -I zone_${zone}_prerouting 1 -t nat -j prerouting_${zone}
}

fw_init() {
	DEFAULTS_APPLIED=

	echo "Loading defaults"
	config_foreach fw_defaults defaults
	echo "Loading zones"
	config_foreach fw_zone zone
	echo "Loading rules"
	config_foreach fw_rule rule
	echo "Loading forwarding"
	config_foreach fw_forwarding forwarding
	echo "Loading redirects"
	config_foreach fw_redirect redirect
	echo "Loading includes"
	config_foreach fw_include include
	uci_set_state firewall core loaded 1
	unset CONFIG_APPEND
	config_load network
	config_foreach fw_addif interface
}

fw_stop() {
	$IPTABLES -F
	$IPTABLES -t mangle -F
	$IPTABLES -t nat -F
	$IPTABLES -t mangle -X
	$IPTABLES -t nat -X
	$IPTABLES -X
	$IPTABLES -P INPUT ACCEPT
	$IPTABLES -P OUTPUT ACCEPT
	$IPTABLES -P FORWARD ACCEPT
	uci_revert_state firewall core
}
