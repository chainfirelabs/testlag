# TestLag

A native Linux C + GTK3 GUI for shaping per-IP/port network latency, jitter,
packet loss, and bandwidth with `tc` (netem). Define multiple rules — each
with interface, source IP/port, destination IP/port, bandwidth, latency,
jitter, and dropped-packet % — and Start/Stop/Delete them. `tc` does the work under the
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
- Closing the window asks for confirmation, and a confirmed exit **stops every
  active rule** before quitting: qdiscs live in the kernel, not in the process,
  so rules left running would keep shaping traffic after TestLag is gone. The
  rules themselves stay in the profile and can be started again next launch.
  If an interface cannot be cleared, the exit says so and names the `tc qdisc
  del` to run by hand. (A crash or `kill` skips this, which is what the
  active-state reconciliation on load is for -- see "Profile".)

## Which direction does a rule shape?

`netem` shapes **egress**: a rule always matches a packet on its way *out* of
the chosen interface. So in every rule, **source is this machine and
destination is the peer** — which is the opposite of how you would describe an
incoming connection.

Traffic *arriving* at this machine is never shaped: that would need an ingress
qdisc and an IFB device, which TestLag does not set up.

That makes "lag the devices hitting my web server" a rule about the **replies**
the server sends:

| what you want | source IP | source port | dest IP | dest port |
| --- | --- | --- | --- | --- |
| Lag everyone using my web server | — | `80` | — | — |
| Lag one device using my web server | — | `80` | that device | — |
| Lag what I fetch from a remote API | — | — | the API's IP | `443` |
| Lag one whole peer, both ways I send | — | — | that peer | — |
| Lag everything leaving this NIC | — | — | — | — |

A field left blank matches anything; all four blank is the all-traffic rule.

Because only the outbound half of a conversation is delayed, a peer sees the
configured latency added **once** per round trip, not twice.

Two things worth knowing when a rule seems to do nothing:

- **An address of this machine is reached over `lo`.** `ip route get <ip>` says
  which interface traffic actually takes; if it prints `local ... dev lo`, a
  rule on the physical NIC will never match, and the rule belongs on `lo`.
- **`tc -s filter show dev <iface>`** prints `(rule hit N success M)` per
  filter. `success 0` means the filter is being evaluated and matching nothing.

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
--src-ip IP       source IP (default: any) — an address of this machine
--src-port PORT   source port (default: any) — e.g. 80 for your own web server
--dst-ip IP       destination IP (default: any); --ip is an alias
--dst-port PORT   destination port (default: any); --port is an alias
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
testlag add --iface lo --dst-ip 192.168.1.50 --dst-port 443 --latency 150 \
    --jitter 30 --drop 5 --bandwidth 100kbit
# lag every device using this machine's web server (delays its replies):
testlag add --iface eth0 --src-port 80 --latency 200
# ...or just one of them:
testlag add --iface eth0 --src-port 80 --dst-ip 192.168.1.77 --latency 200
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
    {"iface":"eth0","src_ip":"","src_port":"","ip":"1.2.3.4","port":"443",
     "bandwidth":"100kbit","latency_ms":100,"jitter_ms":20,"drop_pct":5},
    {"iface":"eth0","src_ip":"","src_port":"80","ip":"","port":"",
     "bandwidth":"","latency_ms":300,"jitter_ms":0,"drop_pct":0}
  ]
}
```

`ip`/`port` are the **destination** match; they keep those names from before
source matching existed, so profiles written by older builds load unchanged
with empty source fields. A rule with all four match fields empty is an
**all-traffic** rule for that interface (no filter). Open/Save buttons in the
UI switch profile files.

A profile file is treated as untrusted input: every rule is validated when it
is loaded, and any rule with a malformed field is dropped (with a warning
naming it) while the rest of the profile still loads.

The profile also records which rules were running when it was saved, but the
qdisc tree lives in the kernel, not in the file. On load, the `active` flags
are reconciled against the live `tc` state (read-only): rules on an interface
that is not currently shaped by TestLag -- after a reboot, say -- are marked
stopped, so the displayed state matches reality and `start` applies them
again. Rules that are still applied (the process was restarted while shaping
was live) stay active.

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
# source matching uses the same filter, on the other side of the packet:
tc filter add dev IF parent 1: protocol ip u32 \
    match ip sport 80 0xffff flowid 1:3
```

- `htb` root (not `prio`) so an unlimited number of per-IP/port rules fit per
  interface.
- With **no** all-traffic rule active, class `1:1` (the htb default) gets a
  `pfifo` pass-through qdisc so unmatched traffic is not dropped.
- Safety: an interface is only managed if its root qdisc is absent, `noqueue`,
  `fq_codel`, or `htb`; a foreign root qdisc is refused with a message.
- The whole tree is applied by a **single** `tc -batch` process rather than one
  `tc` per line: unprivileged, each command would otherwise be its own `sudo`,
  and so its own PAM/logind session (see "Security" below).
- The teardown `tc qdisc del` is skipped when the interface has no root qdisc
  of its own (`noqueue` is the kernel's placeholder, and `htb` goes straight
  over it), so a first Start costs one privileged command in total.
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
- **Few privilege transitions.** An interface is rebuilt by one `tc -batch`
  process that reads its commands from stdin, not by seven to ten separate
  `tc` invocations. Unprivileged each of those was a separate `sudo`, i.e. a
  separate PAM/logind session; in bulk that is enough to exhaust a machine's
  file descriptors. The batch is data on a pipe, never a command line, and no
  validator admits a newline, so a value cannot open a batch line of its own
  (`tc_run_batch()` rejects one outright).
- **No temporary files.** Command output is read from pipes, and the batch is
  written to a pipe, so there is no predictable file in `/tmp` to race.

Running `sudo ./testlag` runs the whole process as root, so the profile file
and its directory should be writable only by you.

## Limitations

- Port filtering is IPv4-only (`u32 match ip sport/dport`); IPv6 rules match
  by address only, and a port set on an IPv6 rule is ignored. A single rule
  cannot mix IPv4 and IPv6 addresses (one `u32` filter is one protocol).
- **Egress only.** Traffic arriving at this machine is not shaped; see "Which
  direction does a rule shape?".
- One all-traffic rule per interface.
- `netem` shapes **egress** (outgoing) traffic on the chosen interface.
- The `htb` "quantum of class is big" warning from the 100 gbit class rate is
  harmless and suppressed (command output is shown only on failure).

## Test

`make test-run` executes `unshare -r -n ./test/test_tc`, which in a private
network namespace verifies: interface discovery, input validators (including
injection rejection and the numeric ranges), whole-rule validation,
netem/filter command builders for source, destination and full four-tuple
matches (plus the IPv4/IPv6 mixing guard), batch mode (including its newline
guard), applying both rule types (state shows `htb` +
both `netem` delays), stopping a specific rule (its delay gone, all-traffic
kept), clearing (back to `noqueue`), the specific-only pass-through path
(unmatched `ping` still succeeds), a full profile save/load round-trip of all
fields, and that a hostile profile file loads its one valid rule while
dropping the injected ones without executing anything.
