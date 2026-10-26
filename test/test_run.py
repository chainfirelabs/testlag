#!/usr/bin/env python3
"""Standalone lifecycle integration checks in an empty network namespace."""
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import tempfile

APP = str(Path(__file__).resolve().parents[1] / 'testlag')


def call(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=15)


def state():
    return call('tc', 'qdisc', 'show', 'dev', 'lo').stdout


assert os.geteuid() == 0 and set(socket.if_nameindex()) == {(1, 'lo')}
assert call('ip', 'link', 'set', 'lo', 'up').returncode == 0
with tempfile.TemporaryDirectory() as tmp:
    profile = Path(tmp) / 'profile.json'
    profile.write_text('intentionally invalid profile: must never be read or written')
    original = profile.read_bytes()
    base = [APP, '--no-sudo', '--profile', str(profile)]
    for direction in ('outgoing', 'incoming', 'both'):
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            p = subprocess.Popen(base + ['run', '--iface', 'lo', '--direction', direction,
                                         '--protocol', 'udp', '--dst-ip', '192.168.1.1', '--dst-port', '12345',
                                         '--latency', '100', '--jitter', '10', '--drop', '1'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(p.stdout, selectors.EVENT_READ)
                    assert selector.select(15), 'startup timed out'
                line = p.stdout.readline()
                assert 'Running standalone' in line, (line, p.stderr.read() if p.poll() is not None else '')
                assert ('htb' if direction == 'outgoing' else 'ingress') in state()
                if direction == 'both':
                    assert 'htb' in state() and 'ingress' in state()
                    outgoing = call('tc', 'filter', 'show', 'dev', 'lo', 'parent', '1:').stdout
                    incoming = call('tc', 'filter', 'show', 'dev', 'tlifb1', 'parent', '1:').stdout
                    assert 'dst_port 12345' in outgoing, outgoing
                    assert 'src_port 12345' in incoming, incoming
                    assert 'dst_ip 192.168.1.1' in outgoing, outgoing
                    assert 'src_ip 192.168.1.1' in incoming, incoming
                busy = call(*(base + ['run', '--iface', 'lo', '--latency', '10']))
                assert busy.returncode == 1 and 'already managed' in busy.stderr
                p.send_signal(sig)
                out, err = p.communicate(timeout=15)
                assert p.returncode == 128 + sig, (p.returncode, err)
                assert 'htb' not in state() and 'ingress' not in state(), state()
                assert profile.read_bytes() == original
            finally:
                if p.poll() is None:
                    p.kill()
                    p.wait()
    for opts in ([], ['--iface', 'lo', '--active'], ['--iface', 'lo', '--latency', '-1'],
                 ['--iface', 'missingiface', '--latency', '1']):
        assert call(*(base + ['run'] + opts)).returncode == 1
        assert 'htb' not in state()
    # Existing profile commands still persist and apply rules normally.
    profile.unlink()
    assert call(*(base + ['add', '--iface', 'lo', '--direction', 'both', '--latency', '20', '--active'])).returncode == 0
    assert 'htb' in state() and 'ingress' in state()
    assert 'both' in call(*(base + ['list'])).stdout
    assert call(*(base + ['stop', 'all'])).returncode == 0
    assert 'htb' not in state()
    assert '20' in call(*(base + ['list'])).stdout
print('Standalone lifecycle and profile regression checks passed')
