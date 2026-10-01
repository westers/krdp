"""Run manually on Sol with the sibling BrokerHostSettingsNativeProbe binary.

Only six disposable user units/config files; no installed root settings, desktop,
PipeWire, device grants or broker changes. No credentials or environment dump.
"""
from pathlib import Path
import hashlib
import os
import shutil
import socket
import subprocess
import tempfile
import uuid

assert socket.gethostname().split('.')[0] == 'sol' and os.getuid() == 1000
probe = Path(__file__).resolve().parent / 'BrokerHostSettingsNativeProbe'
assert probe.is_file() and probe.stat().st_uid == os.getuid()
assert probe.stat().st_mode & 0o022 == 0
environment = dict(os.environ, XDG_RUNTIME_DIR='/run/user/1000',
                   DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
assert Path('/run/user/1000/bus').is_socket()

def brokers():
    return subprocess.check_output(
        ['systemctl', 'show', 'farside-console-host.service', 'farside-virtual-host.service',
         '-p', 'MainPID', '-p', 'ActiveState'], text=True, timeout=10)

original = brokers()
preferences = Path('/home/westers/.config/farsideserverrc')
def preference_hash():
    return hashlib.sha256(preferences.read_bytes()).hexdigest() if preferences.exists() else 'absent'
previous_hash = preference_hash()
base = Path(tempfile.mkdtemp(prefix='farside-t08-format-'))
units = []
try:
    for scope in ('console', 'virtual', 'session'):
        for mode in ('raw', 'edited'):
            config = base / f'{scope}-{mode}.conf'
            payload = subprocess.check_output([str(probe), '--emit', scope, mode], timeout=10)
            with config.open('xb') as file:
                os.chmod(config, 0o600)
                file.write(payload)
            unit = f'farside-t08-format-{uuid.uuid4().hex}.service'
            units.append(unit)
            result = subprocess.run(
                ['systemd-run', '--user', '--quiet', '--wait', '--collect', '--pipe',
                 f'--unit={unit}', '--property=Type=exec',
                 f'--property=EnvironmentFile={config}', '--setenv=FARSIDE_FORMAT_NATIVE=1',
                 str(probe), '--check', scope, mode],
                env=environment, timeout=40)
            assert result.returncode == 0, f'native EnvironmentFile agreement failed: {scope}/{mode}'
finally:
    for unit in units:
        state = subprocess.run(['systemctl', '--user', 'show', unit, '-p', 'ActiveState', '--value'],
                               env=environment, capture_output=True, text=True, timeout=10)
        if state.returncode == 0 and state.stdout.strip() not in ('', 'inactive', 'failed'):
            subprocess.run(['systemctl', '--user', 'stop', unit], env=environment, check=True, timeout=20)
        state = subprocess.run(['systemctl', '--user', 'show', unit, '-p', 'ActiveState', '--value'],
                               env=environment, capture_output=True, text=True, timeout=10)
        assert state.returncode != 0 or state.stdout.strip() in ('', 'inactive', 'failed')
    assert brokers() == original, 'installed broker state changed'
    assert preference_hash() == previous_hash, 'actual preference file changed'
    shutil.rmtree(base)
    print('PRESERVED installed broker state/PIDs and actual preferences; fixture units terminal and files removed', flush=True)
