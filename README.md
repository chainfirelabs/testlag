# TestLag

A native Linux C + GTK3 GUI for shaping per-IP/port network latency, jitter,
packet loss, and bandwidth with `tc` (netem). Define multiple rules — each
with interface, destination IP, port, bandwidth, latency, jitter, and
dropped-packet % — and Start/Stop/Delete them. `tc` does the work under the
hood. Active rules are displayed live, and the rule set is saved to a profile
so the next launch can rerun the same configuration. It also ships a CLI for
managing the same profile and `tc` state without the GUI.

## Build

```sh
make            # builds ./testlag
make test       # builds the headless backend test
make test-run   # runs the backend test in a private network namespace
make clean
```

Dependencies: `gcc`, `pkg-config`, `gtk+-3.0` (GTK3 dev), `glib-2.0`, and
`iproute2` (`tc`, `ip`). On Fedora: `sudo dnf install gcc gtk3-devel iproute2`.

## Run

```sh
./testlag                          # unprivileged; uses sudo for tc writes
sudo ./testlag                     # recommended: no per-command password prompts
./testlag --profile /path/p.json   # custom profile file
./testlag --no-sudo                # never invoke sudo (must already be root)
```

- Unprivileged mode: read commands (`tc qdisc show`, `ip link`) run directly;
  write commands are prefixed with `sudo`, which may prompt for a password on
  the tty.
- Running as `sudo ./testlag` runs everything directly.

## CLI

`testlag` also runs headless — no GUI — to manage the profile and live `tc`
state. The same global options apply (`--profile P`, `--sudo`, `--no-sudo`).
Rule indices are 0-based, as shown by `list`.

```sh
testlag list                 # list rules (index, iface, ip, port, effects, state)
testlag ifaces               # list available network interfaces
testlag add --iface IFACE [rule opts]
testlag edit N [rule opts]   # update rule N (unspecified fields unchanged)
testlag del N                # delete rule N (stops it first if active)
testlag start N | all        # apply rule(s) via tc
testlag stop  N | all        # remove rule(s) from tc
testlag status               # show live tc state for the profile's interfaces
testlag help                 # show this help
```

Rule options for `add`/`edit`:

```
--iface IFACE     network interface (required for add; e.g. lo, eth0)
--ip IP           destination IP (default: any)
--port PORT       destination port (default: any)
--latency MS      latency in milliseconds, 0-60000 (default: 0)
--jitter MS       jitter in milliseconds, 0-60000 (default: 0)
--drop PCT        packet loss percent, 0-100 (default: 0)
--bandwidth RATE  netem rate, e.g. 100kbit, 1mbit (default: unlimited)
--active          (add only) start the rule immediately
```

Semantics mirror the GUI: `start`/`stop` mark the rule active/inactive and
rebuild the whole qdisc tree for its interface from all active rules (idempotent
convergence), so `start all`/`stop all` converge every interface at once. Every
mutating command (`add`, `edit`, `del`, `start`, `stop`) saves the profile
afterwards. `start N` on an already-active rule and `stop N` on a stopped rule
are no-ops.

Examples:

```sh
testlag add --iface lo --ip 192.168.1.50 --port 443 --latency 150 \
    --jitter 30 --drop 5 --bandwidth 100kbit
testlag add --iface eth0 --latency 300 --active   # all-traffic, started now
testlag start 0
testlag stop all
testlag status
testlag del 2
```

## Profile

The rule set is loaded on start and saved on exit (and via the Save button)
as JSON:

```
~/.config/testlag/profile.json  (default; override with --profile P)
```

```json
{
  "version": 1,
  "rules": [
    {"iface":"eth0","ip":"1.2.3.4","port":"443","bandwidth":"100kbit",
     "latency_ms":100,"jitter_ms":20,"drop_pct":5},
    {"iface":"eth0","ip":"","port":"","bandwidth":"","latency_ms":300,
     "jitter_ms":0,"drop_pct":0}
  ]
}
```

A rule with empty `ip` and `port` is an **all-traffic** rule for that
interface (no filter). Open/Save buttons in the UI switch profile files.

A profile file is treated as untrusted input: every rule is validated when it
is loaded, and any rule with a malformed field is dropped (with a warning
naming it) while the rest of the profile still loads.

## How it maps to tc

Each managed interface is rebuilt from scratch on every Start/Stop (idempotent
convergence):

```
tc qdisc del dev IF root                      # teardown
tc qdisc add dev IF root handle 1: htb default 1
tc class add dev IF parent 1: classid 1:1 htb rate 100gbit ceil 100gbit
# all-traffic rule (at most one per interface):
tc qdisc add dev IF parent 1:1 netem delay 300ms
# each specific rule gets its own class + netem + filter (classes 1:2, 1:3, ...):
tc class add dev IF parent 1: classid 1:2 htb rate 100gbit ceil 100gbit
tc qdisc add dev IF parent 1:2 netem delay 100ms 20ms loss 5% rate 100kbit
tc filter add dev IF parent 1: protocol ip u32 \
    match ip dst 1.2.3.4/32 match ip dport 443 0xffff flowid 1:2
```

- `htb` root (not `prio`) so an unlimited number of per-IP/port rules fit per
  interface.
- With **no** all-traffic rule active, class `1:1` (the htb default) gets a
  `pfifo` pass-through qdisc so unmatched traffic is not dropped.
- Safety: an interface is only managed if its root qdisc is absent, `noqueue`,
  `fq_codel`, or `htb`; a foreign root qdisc is refused with a message.
- Commands are never passed to a shell: the command string is split into an
  argv vector and the binary is exec'd directly (see "Security" below).

## Security

TestLag builds `tc` command lines from user input and often runs them as root,
so the execution path is deliberately narrow:

- **No shell.** Commands are split into an argv vector and exec'd directly
  (`g_spawn_sync`), so a value containing `;`, `|`, `` ` `` or `$()` can never
  become a second command, a redirect, or a substitution.
- **No `$PATH` lookup.** `tc`, `ip` and `sudo` are resolved against a fixed
  list of system directories, so an inherited `PATH` cannot decide which
  binary root executes. The child also gets a minimal environment.
- **Untrusted profiles.** Rules loaded from a profile file go through the same
  validators as GUI and CLI input; invalid rules are dropped, never applied.
  `tc_apply_interface()` re-checks every rule as a last gate before exec.
- **No temporary files.** Command output is read from pipes, so there is no
  predictable file in `/tmp` to race.

Running `sudo ./testlag` runs the whole process as root, so the profile file
and its directory should be writable only by you.

## Limitations

- Port filtering is IPv4-only (`u32 match ip dport`); IPv6 rules match by
  destination address only.
- One all-traffic rule per interface.
- `netem` shapes **egress** (outgoing) traffic on the chosen interface.
- The `htb` "quantum of class is big" warning from the 100 gbit class rate is
  harmless and suppressed (command output is shown only on failure).

## Test

`make test-run` executes `unshare -r -n ./test/test_tc`, which in a private
network namespace verifies: interface discovery, input validators (including
injection rejection and the numeric ranges), whole-rule validation,
netem/filter command builders, applying both rule types (state shows `htb` +
both `netem` delays), stopping a specific rule (its delay gone, all-traffic
kept), clearing (back to `noqueue`), the specific-only pass-through path
(unmatched `ping` still succeeds), a full profile save/load round-trip of all
fields, and that a hostile profile file loads its one valid rule while
dropping the injected ones without executing anything.
