#!/usr/bin/env python3
"""Real incoming packet checks, run only inside a private user/network namespace."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

APP = str(Path(__file__).resolve().parents[1] / 'testlag')


def run(*args, ok=True):
    result = subprocess.run(args, capture_output=True, text=True, timeout=15)
    if ok and result.returncode:
        raise AssertionError(f'{args}: {result.stderr}')
    return result


# The peer occupies another network namespace so packets really enter the
# interface under test, instead of taking a local routing shortcut.
PEER = r'''
import json, socket, subprocess, sys, time
print('ready', flush=True)
for line in sys.stdin:
    cmd = json.loads(line)
    if cmd['op'] == 'configure':
        for args in [['ip','link','set','lo','up'],
                     ['ip','addr','add','10.203.0.2/24','dev','lagpeer'],
                     ['ip','-6','addr','add','2001:db8:203::2/64','dev','lagpeer','nodad'],
                     ['ip','link','set','lagpeer','up']]:
            subprocess.run(args, check=True)
        print('configured', flush=True)
        continue
    family = socket.AF_INET6 if cmd.get('ipv6') else socket.AF_INET
    address = '2001:db8:203::1' if cmd.get('ipv6') else '10.203.0.1'
    with socket.socket(family, socket.SOCK_STREAM if cmd['op']=='tcp' else socket.SOCK_DGRAM) as s:
        s.settimeout(5)
        print(time.monotonic(), flush=True)
        if cmd['op'] == 'tcp':
            s.connect((address,cmd['port']))
            s.sendall(b'probe')
        else:
            s.sendto(b'probe',(address,cmd['port']))
'''


def main():
    assert os.geteuid() == 0, 'Run with unshare -r -n'
    # Refuse to touch a namespace containing any existing network interfaces.
    assert set(socket.if_nameindex()) == {(1, 'lo')}, 'Requires an empty network namespace'
    run('ip', 'link', 'set', 'lo', 'up')
    run('ip', 'link', 'add', 'lagtest', 'type', 'veth', 'peer', 'name', 'lagpeer')
    peer = subprocess.Popen(['unshare', '-n', sys.executable, '-u', '-c', PEER],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert peer.stdout.readline().strip() == 'ready'
        run('ip', 'link', 'set', 'lagpeer', 'netns', str(peer.pid))
        run('ip', 'addr', 'add', '10.203.0.1/24', 'dev', 'lagtest')
        run('ip', '-6', 'addr', 'add', '2001:db8:203::1/64', 'dev', 'lagtest', 'nodad')
        run('ip', 'link', 'set', 'lagtest', 'up')
        peer.stdin.write(json.dumps({'op': 'configure'}) + '\n'); peer.stdin.flush()
        assert peer.stdout.readline().strip() == 'configured'
        ifb = f'tlifb{socket.if_nametoindex("lagtest"):x}'
        baseline = run('tc', 'qdisc', 'show', 'dev', 'lagtest').stdout

        def packet(proto='udp', port=18080, ipv6=False):
            with socket.socket(socket.AF_INET6 if ipv6 else socket.AF_INET,
                               socket.SOCK_STREAM if proto == 'tcp' else socket.SOCK_DGRAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind(('2001:db8:203::1' if ipv6 else '10.203.0.1', port))
                server.settimeout(5)
                if proto == 'tcp': server.listen(1)
                peer.stdin.write(json.dumps({'op': proto, 'port': port, 'ipv6': ipv6}) + '\n')
                peer.stdin.flush()
                sent = float(peer.stdout.readline())
                if proto == 'tcp':
                    conn, _ = server.accept()
                    with conn: assert conn.recv(64) == b'probe'
                else: assert server.recv(64) == b'probe'
                return time.monotonic() - sent

        # Warm up ARP/NDP before timing comparisons.
        packet(); packet(ipv6=True)
        with tempfile.TemporaryDirectory(prefix='testlag-ingress-test-') as tmp:
            profile = str(Path(tmp) / 'profile.json')

            def app(*args, ok=True):
                return run(APP, '--no-sudo', '--profile', profile, *args, ok=ok)

            app('add', '--iface', 'lagtest', '--direction', 'incoming', '--protocol', 'udp',
                '--src-ip', '10.203.0.99/24', '--dst-port', '18080', '--latency', '200', '--active')
            assert baseline.strip() in run('tc', 'qdisc', 'show', 'dev', 'lagtest').stdout
            delayed = packet()
            other_port = packet(port=18081)
            other_protocol = packet(proto='tcp')
            assert delayed >= .17, delayed
            assert delayed - max(other_port, other_protocol) >= .10, (delayed, other_port, other_protocol)
            print(f'Incoming UDP/CIDR: {delayed:.3f}s; other port {other_port:.3f}s; TCP {other_protocol:.3f}s')

            app('add', '--iface', 'lagtest', '--direction', 'incoming', '--protocol', 'udp',
                '--src-ip', '2001:db8:203::99/64', '--dst-port', '18080', '--latency', '200', '--active')
            assert packet(ipv6=True) >= .17
            # A fresh CLI process must recognize the existing incoming rule.
            data = json.loads(Path(profile).read_text())
            assert all(r['active'] for r in data['rules'])
            app('stop', '0')
            assert packet() < .15
            assert packet(ipv6=True) >= .17
            app('stop', '1')
            assert not run('ip', 'link', 'show', 'dev', ifb, ok=False).returncode == 0
            assert run('tc', 'qdisc', 'show', 'dev', 'lagtest').stdout == baseline

            # Independent outgoing and incoming trees; stopping incoming keeps outgoing.
            app('start', '0')
            app('add', '--iface', 'lagtest', '--direction', 'outgoing', '--protocol', 'tcp',
                '--dst-port', '19000', '--latency', '20', '--active')
            app('stop', '0')
            assert 'htb 1: root' in run('tc', 'qdisc', 'show', 'dev', 'lagtest').stdout
            assert run('ip', 'link', 'show', 'dev', ifb, ok=False).returncode != 0
            app('stop', 'all')
            assert run('tc', 'qdisc', 'show', 'dev', 'lagtest').stdout == baseline

            # A user's pre-existing ingress configuration is left untouched.
            run('tc', 'qdisc', 'add', 'dev', 'lagtest', 'ingress')
            foreign = run('tc', 'qdisc', 'show', 'dev', 'lagtest').stdout
            assert app('start', '0', ok=False).returncode != 0
            assert run('tc', 'qdisc', 'show', 'dev', 'lagtest').stdout == foreign
            assert run('ip', 'link', 'show', 'dev', ifb, ok=False).returncode != 0
            run('tc', 'qdisc', 'del', 'dev', 'lagtest', 'ingress')

            run('ip', 'link', 'add', ifb, 'type', 'ifb')
            assert app('start', '0', ok=False).returncode != 0
            assert run('ip', 'link', 'show', 'dev', ifb).returncode == 0
            run('ip', 'link', 'del', ifb)
            print('IPv6, selective stop, outgoing isolation, cleanup and foreign-resource protection passed.')
    finally:
        peer.terminate(); peer.wait(timeout=5)


if __name__ == '__main__':
    main()
