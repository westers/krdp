"""Root/Sol only: inspect installed hosts without mutation and a private bus.
Passwords arrive only at sudo in the external launcher, never this fixture.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import socket
import subprocess

assert not os.getuid() and not os.geteuid() and socket.gethostname().split('.')[0] == 'sol'
upload = Path(__file__).resolve().parent
base = Path('/tmp/farside-t08-runtime-root')
assert not base.exists()
manifest = dict((name, digest) for digest, name in (line.split() for line in (upload / 'SHA256SUMS').read_text().splitlines()))
for name in ('BrokerHostRuntimeNativeTest', 'farside-host-settings-helper'):
    source = upload / name
    assert source.is_file() and not source.is_symlink() and source.stat().st_uid == 1000 and not source.stat().st_mode & 0o022
    assert hashlib.sha256(source.read_bytes()).hexdigest() == manifest[name]

policy = Path('/etc/farside')
def state():
    result = {}
    for path in policy.iterdir():
        if path.is_file():
            info = path.lstat()
            result[path.name] = (hashlib.sha256(path.read_bytes()).hexdigest(), info.st_uid, info.st_mode, info.st_ino, path.is_symlink())
        else:
            result[path.name] = ('directory', path.lstat().st_uid, path.lstat().st_mode)
    pref = Path('/home/westers/.config/farsideserverrc')
    result['user-preferences'] = hashlib.sha256(pref.read_bytes()).hexdigest() if pref.exists() else 'absent'
    return result

def brokers():
    return subprocess.check_output(['systemctl', 'show', 'farside-console-host.service',
        'farside-virtual-host.service', '-p', 'MainPID', '-p', 'ActiveState'], text=True, timeout=10)

before, previous_brokers = state(), brokers()
environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'HOME': '/root', 'FARSIDE_RUNTIME_PRIVATE_BUS': '1'}
try:
    base.mkdir(mode=0o700)
    for name in ('BrokerHostRuntimeNativeTest', 'farside-host-settings-helper'):
        destination = base / name
        shutil.copyfile(upload / name, destination)
        destination.chmod(0o755)
        assert hashlib.sha256(destination.read_bytes()).hexdigest() == manifest[name]
    tested = subprocess.run(['dbus-run-session', '--', str(base / 'BrokerHostRuntimeNativeTest')],
        env=environment, capture_output=True, timeout=60)
    assert b'fixture-secret-only' not in tested.stdout + tested.stderr and b'PRIVATE KEY' not in tested.stdout + tested.stderr
    print(tested.stdout.decode(), end='', flush=True)
    if tested.stderr:
        print(tested.stderr.decode(), end='', flush=True)
    assert tested.returncode == 0, 'native runtime suite failed'
    for scope in ('console', 'virtual'):
        request = {'version': 1, 'operation': 'inspect-runtime', 'scope': scope}
        inspected = subprocess.run([str(base / 'farside-host-settings-helper')],
            input=json.dumps(request).encode(), env=environment, capture_output=True, timeout=25)
        assert inspected.returncode == 0, 'actual read-only helper inspection failed'
        value = json.loads(inspected.stdout)['runtime']
        assert value['unit'] == 'farside-' + scope + '-host.service' and value['scope'] == scope
        assert value['pid'] > 1 and value['runningVerified'], 'actual process was not verified'
        assert value['running']['Port'] == ('3391' if scope == 'console' else '3395')
        assert b'PRIVATE KEY' not in inspected.stdout + inspected.stderr
        print('PASS actual fixed helper read-only installed', scope, 'startup settings verified', flush=True)
    refused = subprocess.run([str(base / 'farside-host-settings-helper')],
        input=json.dumps({'version': 1, 'operation': 'inspect-runtime', 'scope': 'session'}).encode(),
        env=environment, capture_output=True, timeout=5)
    assert refused.returncode != 0 and 'error' in json.loads(refused.stdout)
    print('PASS session grant scope cannot imply retained-desktop runtime application', flush=True)
finally:
    assert state() == before and brokers() == previous_brokers, 'installed host state changed'
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            assert str(base).encode() not in (proc / 'cmdline').read_bytes(), 'runtime fixture process remains'
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            pass
    if base.exists():
        shutil.rmtree(base)
    print('PRESERVED all real policy file hashes/owners/modes/inodes/directory entries, user preferences and broker PIDs; own root fixture removed', flush=True)
