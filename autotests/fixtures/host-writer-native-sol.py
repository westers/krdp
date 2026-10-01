"""Manual root filesystem fixture on Sol. Run from a fresh user upload directory
with the sibling helper binary and its SHA256SUMS. Never register in Hal CTest.
The actual root filesystem is read-only inside each helper namespace. Only the
private policy bind is writable. Real configuration hashes and broker PIDs are
checked before/after; no credentials, TLS key bytes or raw replies are logged.
"""
from pathlib import Path
import copy
import fcntl
import hashlib
import json
import os
import shutil
import socket
import stat
import subprocess
import time

assert os.geteuid() == 0 and socket.gethostname().split('.')[0] == 'sol'
upload = Path(__file__).resolve().parent
source = upload / 'farside-host-settings-helper'
assert source.is_file() and not source.is_symlink() and source.stat().st_uid == 1000
assert source.stat().st_mode & 0o022 == 0
expected_hash = (upload / 'SHA256SUMS').read_text().split()[0]
assert hashlib.sha256(source.read_bytes()).hexdigest() == expected_hash
base = Path('/tmp/farside-t08-host-root')
assert not base.exists()
real = [Path('/etc/farside') / name for name in
        ('console-host.conf', 'virtual-host.conf', 'virtual-session.conf', 'authentication.json',
         'console.crt', 'console.key', 'virtual-host.crt', 'virtual-host.key')]
real.append(Path('/home/westers/.config/farsideserverrc'))

def hashes():
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else 'absent'
            for path in real}

def brokers():
    return subprocess.check_output(['systemctl', 'show', 'farside-console-host.service',
                                    'farside-virtual-host.service', '-p', 'MainPID', '-p', 'ActiveState'],
                                   text=True, timeout=10)

previous, previous_brokers = hashes(), brokers()
passes = 0

def check(label):
    global passes
    passes += 1
    print('PASS', label, flush=True)

try:
    base.mkdir(mode=0o700)
    policy = base / 'policy'
    policy.mkdir(mode=0o700)
    binary = base / 'helper'
    shutil.copyfile(source, binary)
    binary.chmod(0o755)  # parent0700 outside; UID refusal tested via a private mount alias
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == expected_hash
    command = ['bwrap', '--unshare-all', '--die-with-parent', '--ro-bind', '/', '/',
               '--tmpfs', '/tmp', '--ro-bind', str(binary), '/tmp/helper',
               '--bind', str(policy), '/etc/farside', '--proc', '/proc', '--dev', '/dev']
    if Path('/dev/dri').exists():
        command += ['--ro-bind', '/dev/dri', '/dev/dri']
    for name in ('nvidia0', 'nvidiactl', 'nvidia-uvm'):
        path = Path('/dev') / name
        if path.exists():
            command += ['--ro-bind', str(path), str(path)]
    command += ['--clearenv', '--setenv', 'PATH', '/usr/bin:/bin', '--setenv', 'LANG', 'C.UTF-8']

    def invoke(request, success=True, extra=(), uid=None, raw=None):
        cmd = command + ([] if uid is None else ['--uid', str(uid), '--gid', str(uid)]) + ['/tmp/helper'] + list(extra)
        result = subprocess.run(cmd, input=raw if raw is not None else json.dumps(request).encode(),
                                capture_output=True, timeout=25)
        assert (result.returncode == 0) == success, 'unexpected helper status'
        assert b'PRIVATE KEY' not in result.stdout and b'fixture-secret-only' not in result.stdout, 'public secret leak'
        assert b'PRIVATE KEY' not in result.stderr and b'fixture-secret-only' not in result.stderr, 'diagnostic secret leak'
        reply = json.loads(result.stdout)
        assert ('error' in reply) != success, 'incorrect helper reply'
        return reply

    def read(scope):
        return invoke({'version': 1, 'operation': 'read', 'scope': scope})['snapshot']

    def save(snapshot, values=None, tls=None, success=True):
        request = {'version': 1, 'operation': 'save', 'scope': snapshot['scope'],
                   'revision': snapshot['revision'], 'values': snapshot['values'] if values is None else values}
        if tls is not None:
            request['tls'] = tls
        return invoke(request, success)

    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def pair(name):
        cert, key = policy / f'{name}.crt', policy / f'{name}.key'
        result = subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt',
                                 'ec_paramgen_curve:P-256', '-keyout', str(key), '-out', str(cert),
                                 '-nodes', '-subj', '/CN=farside-fixture-only', '-days', '365'],
                                capture_output=True, timeout=15)
        assert result.returncode == 0
        key.chmod(0o600)
        cert.chmod(0o644)
        return cert, key

    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False, extra=['--help'])
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False, uid=1000)
    assert not list(policy.iterdir())
    check('arguments and nonroot entry refused before any policy access')
    for scope in ('console', 'virtual', 'session'):
        snapshot = read(scope)
        assert snapshot['values'] == {} and snapshot['runtimeVerified'] is False
        assert snapshot['application'] == ('new-desktops' if scope == 'session' else 'broker-restart')
    check('absent scopes inherit exact defaults without creating settings or claiming runtime application')

    console = policy / 'console-host.conf'
    console.write_text('# retained\nCUSTOM_EXECUTION="fixture-secret-only"\nFARSIDE_CONSOLE_PORT=4321\n')
    console.chmod(0o600)
    virtual = policy / 'virtual-host.conf'
    virtual.write_text('FARSIDE_VIRTUAL_PORT=4567\n')
    virtual.chmod(0o600)
    session = policy / 'virtual-session.conf'
    session.write_text('FARSIDE_VIRTUAL_VAAPI_DRIVER=auto\n')
    session.chmod(0o600)
    virtual_before, session_before = digest(virtual), digest(session)
    original = read('console')
    values = {**original['defaults'], 'Port': '5432', 'Quality': '91', 'AdaptiveQuality': 'true',
              'PreferAudioQuality': 'true', 'SoftwareEncoding': 'never', 'Av1Tiles': '8', 'VaapiDriver': 'off'}
    saved = save(original, values)
    assert saved['saved'] and saved['restartRequired'] and not saved['newDesktopRequired']
    assert saved['snapshot']['values'] == values and len(values) == 12
    assert b'CUSTOM_EXECUTION="fixture-secret-only"\n' in console.read_bytes()
    assert console.stat().st_uid == 0 and stat.S_IMODE(console.stat().st_mode) == 0o600
    assert digest(virtual) == virtual_before and digest(session) == session_before
    check('all Console fields atomically roundtrip root0600 with unrelated private bytes and other scopes preserved')
    snapshot = saved['snapshot']
    before = digest(console)
    save(original, values, success=False)
    assert digest(console) == before
    console.write_bytes(console.read_bytes() + b'# nonpreference admin edit\n')
    modified = digest(console)
    save(snapshot, values, success=False)
    assert digest(console) == modified
    check('both stale preferences and unrelated administrator changes refused unchanged')

    for mutation in ({'path': '/etc/arbitrary'}, {'scope': '../console'}, {'version': 2},
                     {'values': {'Port': '5432\nINJECTION=fixture-secret-only'}},
                     {'values': {'Worker': '/bin/sh'}}, {'values': {'Port': 5432}},
                     {'tls': {'mode': 'keep', 'path': '/etc/arbitrary'}}):
        current = read('console')
        request = {'version': 1, 'operation': 'save', 'scope': 'console',
                   'revision': current['revision'], 'values': current['values'], **mutation}
        before = digest(console)
        invoke(request, False)
        assert digest(console) == before
    invoke({}, False, raw=b'{} broken JSON')
    invoke({}, False, raw=b'x' * 262145)
    check('caller paths, raw environment injection, unknown fields, types, malformed and oversized input refused')

    backup = policy / 'original.conf'
    console.rename(backup)
    console.symlink_to(backup.name)
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    console.unlink()
    os.link(backup, console)
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    console.unlink()
    os.mkfifo(console, 0o600)
    started = time.monotonic()
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    assert time.monotonic() - started < 3
    console.unlink()
    backup.rename(console)
    check('final symlink, hardlink and FIFO refused without following or blocking')
    console.chmod(0o666)
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    console.chmod(0o600)
    os.chown(console, 1000, 1000)
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    os.chown(console, 0, 0)
    policy.chmod(0o777)
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    policy.chmod(0o700)
    check('foreign owner, writable file and unsafe settings directory refused')
    lock = policy / '.host-settings-console.lock'
    lock.unlink()
    lock.symlink_to(console.name)
    invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    lock.unlink()
    read('console')
    with lock.open('r+') as file:
        fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        invoke({'version': 1, 'operation': 'read', 'scope': 'console'}, False)
    check('symlink lock and concurrent cooperating writer refused')
    console.chmod(0o644)
    current = read('console')
    save(current)
    assert stat.S_IMODE(console.stat().st_mode) == 0o600
    check('unchanged save preserves bytes and makes an existing safe environment file private')

    std_cert, std_key = pair('console')
    custom_cert, custom_key = pair('custom')
    standard_hashes = digest(std_cert), digest(std_key)
    current = read('console')
    values = dict(current['values'], Certificate='/etc/farside/custom.crt', CertificateKey='/etc/farside/custom.key')
    before = digest(console)
    save(current, values, success=False)
    assert digest(console) == before
    selected = save(current, values, {'mode': 'existing'})['snapshot']
    assert selected['tls']['state'] == 'valid' and not selected['tls']['administratorManaged']
    fingerprint = subprocess.check_output(['openssl', 'x509', '-in', str(custom_cert), '-noout', '-fingerprint', '-sha256'], text=True).strip().split('=', 1)[1]
    assert selected['tls']['fingerprint'] == fingerprint
    check('explicit existing safe TLS pair selection verifies independent fingerprint without modifying material')
    std_cert_before = digest(custom_cert), digest(custom_key)
    for kind in ('writable-key', 'foreign-key', 'arbitrary-path', 'missing-path'):
        current = read('console')
        values = dict(current['values'])
        if kind == 'writable-key':
            custom_key.chmod(0o644)
        elif kind == 'foreign-key':
            os.chown(custom_key, 1000, 1000)
        elif kind == 'arbitrary-path':
            values['Certificate'] = '/etc/farside/console-host.conf'
        else:
            values['CertificateKey'] = '/etc/farside/nonexistent.key'
        before = digest(console)
        save(current, values, {'mode': 'existing'}, success=False)
        assert digest(console) == before
        custom_key.chmod(0o600)
        os.chown(custom_key, 0, 0)
    assert (digest(custom_cert), digest(custom_key)) == std_cert_before
    check('unsafe/private-key owner/mode, arbitrary non-TLS and missing custom TLS paths refused without writes')
    unsafe = policy / 'unsafe-parent'
    unsafe.mkdir(mode=0o777)
    unsafe.chmod(0o777)
    shutil.copyfile(custom_cert, unsafe / 'certificate.crt')
    shutil.copyfile(custom_key, unsafe / 'private.key')
    (unsafe / 'private.key').chmod(0o600)
    current = read('console')
    values = dict(current['values'], Certificate='/etc/farside/unsafe-parent/certificate.crt',
                  CertificateKey='/etc/farside/unsafe-parent/private.key')
    before = digest(console)
    save(current, values, {'mode': 'existing'}, success=False)
    assert digest(console) == before
    unsafe.chmod(0o700)
    check('an otherwise valid TLS pair under a writable ancestor cannot authorize broker writes')
    alias_cert, alias_key = policy / 'admin.crt', policy / 'admin.key'
    alias_cert.symlink_to(custom_cert.name)
    alias_key.symlink_to(custom_key.name)
    current = read('console')
    values = dict(current['values'], Certificate='/etc/farside/admin.crt', CertificateKey='/etc/farside/admin.key')
    selected = save(current, values, {'mode': 'existing'})['snapshot']
    assert selected['tls']['administratorManaged'] and selected['tls']['fingerprint'] == fingerprint
    check('existing root administrator-managed TLS symlinks retain pair identity and renewal ownership')

    tls = {'mode': 'import', 'certificatePem': custom_cert.read_text(), 'privateKeyPem': custom_key.read_text()}
    for field in ('certificatePem', 'privateKeyPem'):
        current = read('console')
        values = {key: value for key, value in current['values'].items() if key not in ('Certificate', 'CertificateKey')}
        malformed = {**tls, field: 'fixture-secret-only'}
        before = digest(console)
        save(current, values, malformed, success=False)
        assert digest(console) == before and not (policy / 'tls-imports').exists()
    other_cert, other_key = pair('other')
    current = read('console')
    values = {key: value for key, value in current['values'].items() if key not in ('Certificate', 'CertificateKey')}
    save(current, values, {**tls, 'privateKeyPem': other_key.read_text()}, success=False)
    encrypted_key = policy / 'encrypted.key'
    result = subprocess.run(['openssl', 'pkey', '-in', str(custom_key), '-aes-256-cbc',
                             '-passout', 'stdin', '-out', str(encrypted_key)], input=b'fixture-only-passphrase\n',
                            capture_output=True, timeout=10)
    assert result.returncode == 0
    save(current, values, {**tls, 'privateKeyPem': encrypted_key.read_text()}, success=False)
    assert not (policy / 'tls-imports').exists()
    check('malformed, mismatched and encrypted imports refused noninteractively with no staged generations')

    imported = save(current, values, tls)['snapshot']
    assert imported['tls']['administratorManaged'] and imported['tls']['fingerprint'] == fingerprint
    prefix = '/etc/farside/'
    key_path = policy / imported['effective']['CertificateKey'].removeprefix(prefix)
    cert_path = policy / imported['effective']['Certificate'].removeprefix(prefix)
    assert key_path.is_symlink() and cert_path.is_symlink()
    assert key_path.read_bytes() == custom_key.read_bytes() and cert_path.read_bytes() == custom_cert.read_bytes()
    assert key_path.resolve().stat().st_uid == 0 and stat.S_IMODE(key_path.resolve().stat().st_mode) == 0o600
    assert stat.S_IMODE(key_path.parent.stat().st_mode) == 0o700
    assert b'PRIVATE KEY' not in console.read_bytes()
    first_generation = key_path.parent
    first_digest = digest(key_path)
    check('TLS import stages a private complete pair then atomically publishes two managed paths without key disclosure')
    next_tls = {'mode': 'import', 'certificatePem': other_cert.read_text(), 'privateKeyPem': other_key.read_text()}
    next_values = {key: value for key, value in imported['values'].items() if key not in ('Certificate', 'CertificateKey')}
    second = save(imported, next_values, next_tls)['snapshot']
    assert second['effective']['CertificateKey'] != imported['effective']['CertificateKey']
    assert first_generation.exists() and digest(key_path) == first_digest
    assert (digest(std_cert), digest(std_key)) == standard_hashes
    check('a second import uses a new generation and preserves material referenced by an existing host')
    values = {key: value for key, value in second['values'].items() if key not in ('Certificate', 'CertificateKey')}
    standard = save(second, values, {'mode': 'standard'})['snapshot']
    assert standard['effective']['Certificate'] == '/etc/farside/console.crt'
    assert (digest(std_cert), digest(std_key)) == standard_hashes and first_generation.exists()
    check('standard TLS path reset preserves original standard fingerprint and every old import generation')
    generations = set((policy / 'tls-imports/console').iterdir())
    before = digest(console)
    request = {'version': 1, 'operation': 'save', 'scope': 'console', 'revision': standard['revision'],
               'values': standard['values'], 'tls': tls}
    readonly = command + ['--remount-ro', '/etc/farside', '--bind', str(policy / 'tls-imports'),
                          '/etc/farside/tls-imports', '/tmp/helper']
    result = subprocess.run(readonly, input=json.dumps(request).encode(), capture_output=True, timeout=15)
    assert result.returncode == 1 and 'atomically' in json.loads(result.stdout)['error']
    assert digest(console) == before and set((policy / 'tls-imports/console').iterdir()) == generations
    assert digest(key_path) == first_digest
    check('config commit failure after successful TLS staging removes only the unpublished generation')
    (policy / 'tls-imports').chmod(0o777)
    save(standard, standard['values'], tls, success=False)
    assert digest(console) == before and set((policy / 'tls-imports/console').iterdir()) == generations
    (policy / 'tls-imports').chmod(0o700)
    check('an unsafe fixed import subtree refuses writes and preserves all old generations')

    current = read('virtual')
    values = {**current['defaults'], 'Port': '5678', 'Quality': '92', 'StandardClientMedia': 'false'}
    saved = save(current, values)['snapshot']
    assert saved['values'] == values and len(values) == 11
    assert saved['cameraLoopback']['supported'] is False
    before = digest(virtual)
    save(saved, dict(values, CameraLoopbackDevice='/dev/video0'), success=False)
    assert digest(virtual) == before
    current = read('console')
    before = digest(console)
    save(current, dict(current['values'], CameraLoopbackDevice='/dev/null'), success=False)
    save(current, dict(current['values'], CameraLoopbackDevice='/dev/video999999'), success=False)
    assert digest(console) == before
    check('all Virtual host fields roundtrip; invalid Console and unavailable Virtual loopback selections cannot appear applied')

    current = read('session')
    assert current['renderDevices'], 'quiet Sol supported GPU inventory missing'
    assert any(item['driver'] == 'nvidia' for item in current['renderDevices']), 'NVIDIA complete device set absent'
    pci = current['renderDevices'][0]['pci']
    saved = save(current, {'RenderPci': pci, 'VaapiDriver': 'off'})
    assert saved['newDesktopRequired'] and not saved['restartRequired']
    assert saved['snapshot']['values'] == {'RenderPci': pci, 'VaapiDriver': 'off'}
    assert stat.S_IMODE(session.stat().st_mode) == 0o600
    before = digest(session)
    save(saved['snapshot'], {'RenderPci': '0000:ff:00.0', 'VaapiDriver': 'auto'}, success=False)
    assert digest(session) == before
    empty = save(saved['snapshot'], {})['snapshot']
    assert empty['effective']['RenderPci'] == '' and empty['values'] == {}
    check('shared production NVIDIA device authority accepts stable PCI grant, rejects absent device, and resets to no grant for new desktops')

    process = subprocess.Popen(command + ['/tmp/helper'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        process.wait(timeout=13)
        output = process.stdout.read()
        assert process.returncode == 1 and 'timed out' in json.loads(output)['error']
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdin.close()
    check('unclosed stdin has a bounded real helper deadline')
    print(f'{passes} native root helper checks passed', flush=True)
finally:
    assert hashes() == previous, 'actual configuration or TLS material changed'
    assert brokers() == previous_brokers, 'installed broker state changed'
    if base.exists():
        shutil.rmtree(base)
    print('PRESERVED actual host/auth/user/TLS hashes and installed broker PIDs; all private namespaces exited and root fixture removed', flush=True)
