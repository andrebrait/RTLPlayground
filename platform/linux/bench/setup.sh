#!/bin/sh
# Creates the bench: namespace `sw` (where the daemon runs) with lan1..lan6, each the veth peer
# of h<N> in its own namespace host<N> (192.0.2.1<N>/24, TEST-NET-1). Needs root.
# Fails if one of these names already exists: run teardown.sh first.
set -e
ip netns add sw
ip -n sw link set lo up
for n in 1 2 3 4 5 6; do
	ip netns add host$n
	ip link add lan$n netns sw type veth peer name h$n netns host$n
	ip -n sw link set lan$n up
	ip -n host$n link set lo up
	ip -n host$n addr add 192.0.2.1$n/24 dev h$n
	ip -n host$n link set h$n up
done
