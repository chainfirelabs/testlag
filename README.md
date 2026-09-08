<p align="center">
  <img src="docs/testlag-screenshot.png" alt="TestLag application with a sample incoming UDP rule matching a CIDR subnet" width="1060">
</p>

<h1 align="center">TestLag</h1>

<p align="center">
  <em>Add latency, jitter, packet loss and bandwidth limits to specific network
  flows on Linux — from a GUI or a CLI.</em>
</p>

## About

TestLag is a front-end over `tc`/netem for testing software on a bad network
without degrading your whole machine. You define rules — *this interface, this
flow, 300 ms of latency, 5 % loss* — and start and stop them individually.
Traffic that does not match a rule is untouched, so one API endpoint can crawl
while SSH stays fast.

## Features

- Latency, jitter, packet loss and bandwidth limits, per rule
- Match flows by direction, protocol, source/destination IP (or CIDR) and port
- Any number of rules per interface, started and stopped independently
- GTK3 GUI and a headless CLI over the same profile and the same `tc` state
- Live view of the qdisc and filter tree
- Rules persist as JSON in `~/.config/testlag/profile.json`

## Install

Install the build and runtime dependencies:

```sh
# Fedora / RHEL
sudo dnf install gcc make pkgconf-pkg-config gtk3-devel glib2-devel iproute iproute-tc

# Debian / Ubuntu
sudo apt install build-essential pkg-config libgtk-3-dev libglib2.0-dev iproute2

# Arch
sudo pacman -S --needed base-devel pkgconf gtk3 glib2 iproute2
```

Then build:

```sh
git clone <this repository>
cd testlag
make                # builds ./testlag
```

There is no install step. To put it on your `PATH`:

```sh
sudo install -m 0755 testlag /usr/local/bin/testlag
```

Other targets: `make test`, `make test-run`, `make clean`.

## Quick start

```sh
# 300 ms of latency on everything leaving eth0
sudo ./testlag add --iface eth0 --latency 300 --active

sudo ./testlag list                 # see the rule and its state
sudo ./testlag status               # see the qdisc tree it built
sudo ./testlag stop all             # put the interface back
```

Or run `./testlag` with no arguments for the GUI: pick an interface, fill in
the effects, **Add**, then **Start**.

## Run

```sh
./testlag                          # GUI; opens a password dialog when needed
sudo ./testlag                     # run the whole app as root
./testlag --profile /path/p.json   # custom profile file
```

CLI commands:

```sh
testlag list                 # list rules (index, iface, match, effects, state)
testlag ifaces               # list available network interfaces
testlag add --iface IFACE [rule opts]
testlag edit N [rule opts]   # update rule N
testlag del N                # delete rule N
testlag start N | all        # apply rule(s) via tc
testlag stop  N | all        # remove rule(s) from tc
testlag status               # show live tc state
testlag help                 # show full option list
```

Rule options for `add`/`edit`:

```
--iface IFACE       interface (required for add; e.g. lo, eth0)
--direction DIR     outgoing (default) or incoming
--protocol PROTO    any (default), tcp, udp, sctp, icmp, icmpv6
--src-ip IP         source IP or CIDR (default: any)
--src-port PORT     source port (default: any)
--dst-ip IP         destination IP or CIDR (default: any)
--dst-port PORT     destination port (default: any)
--latency MS        latency in milliseconds, 0-60000
--jitter MS         jitter in milliseconds, 0-60000
--drop PCT          packet loss percent, 0-100
--bandwidth RATE    rate limit, e.g. 100kbit, 1mbit
--active            (add only) start the rule immediately
```

Rules shape **outgoing** traffic by default, so "lag everyone using my web
server" matches the replies it sends (`--src-port 80`). Use
`--direction incoming` for the other side. If a rule seems to do nothing,
check `ip route get <ip>` — `local ... dev lo` means the traffic never leaves
the machine, and the rule belongs on `lo`.

Closing the GUI stops every active rule before quitting; the rules stay in the
profile and can be started again next launch.

## License

[MIT](LICENSE) © 2026 ChainFire Labs.
