# Test-only subprocess fixture. Never reads or writes installed host settings.
import json
import hashlib
import os
import pathlib
import sys
import time

import subprocess

base = pathlib.Path(sys.argv[1])
sub = os.environ.get('FIXTURE_SUB') == '1'
request = json.load(sys.stdin)
assert all('PRIVATE KEY' not in value for value in sys.argv)
if not sub:
    # One line per helper process: tests count authorizations/invocations.
    with open(base / 'invocations', 'a') as log:
        log.write(request['operation'] + '\n')
scope = request.get('scope')
# A batch entry reads its own per-scope mode; everything else uses the global one.
mode_file = base / ('mode-' + scope if sub else 'mode')
mode = mode_file.read_text().strip() if mode_file.exists() else 'success'
if request['operation'] == 'save-batch':
    assert set(request) == {'version', 'operation', 'requests'}
    if mode in ('cancel', 'denied'):
        sys.exit(126 if mode == 'cancel' else 127)
    if mode == 'malformed':
        print('fixture-private-diagnostic')
        sys.exit(0)
    if mode == 'crash':
        os.kill(os.getpid(), 9)
    if mode == 'timeout':
        time.sleep(3)
    results = []
    for entry in request['requests']:
        done = subprocess.run([sys.executable, __file__, str(base)], input=json.dumps(entry), capture_output=True,
                              text=True, env=dict(os.environ, FIXTURE_SUB='1'))
        results.append({'scope': entry['scope'], 'status': done.returncode, 'reply': json.loads(done.stdout)})
    print(json.dumps({'results': results}))
    sys.exit(0)
assert scope in ('console', 'virtual', 'session')
defaults = {'RenderPci': '', 'VaapiDriver': 'auto'} if scope == 'session' else {
    'Address': '0.0.0.0', 'Port': '3391' if scope == 'console' else '3395',
    'Certificate': '/etc/farside/console.crt' if scope == 'console' else '/etc/farside/virtual-host.crt',
    'CertificateKey': '/etc/farside/console.key' if scope == 'console' else '/etc/farside/virtual-host.key',
    'Quality': '80', 'AdaptiveQuality': 'false', 'PreferAudioQuality': 'false',
    'StandardClientMedia': 'true', 'CameraLoopbackDevice': 'none', 'SoftwareEncoding': 'auto', 'SoftwareAvc': 'auto', 'SoftwareHevc': 'auto', 'SoftwareAv1': 'auto', 'Av1Tiles': 'auto'}
if scope == 'console':
    defaults['VaapiDriver'] = 'auto'
state_file = base / (scope + '.json')
state = json.loads(state_file.read_text()) if state_file.exists() else {'revision': 'a' * 64, 'values': {}}
if mode in ('cancel', 'denied'):
    sys.exit(126 if mode == 'cancel' else 127)
if mode == 'timeout':
    time.sleep(3)
if mode == 'oversized':
    print('x' * 300000, flush=True)
    sys.exit(0)
if mode == 'stderr':
    sys.stderr.write('fixture-private-diagnostic' * 10000)
    sys.stderr.flush()
if mode == 'malformed':
    print('fixture-private-diagnostic')
    sys.exit(0)
if request['operation'] == 'inspect-runtime':
    assert scope != 'session' and set(request) == {'version', 'operation', 'scope'}
    effective = defaults | state['values']
    runtime = {'version': 1, 'scope': scope, 'unit': 'farside-' + scope + '-host.service',
               'storedRevision': state['revision'], 'state': 'verified', 'loadState': 'loaded',
               'activeState': 'active', 'subState': 'running', 'pid': 42, 'custom': False,
               'needsReload': False, 'configuredVerified': True, 'runningVerified': True,
               'configured': effective.copy(), 'running': effective.copy(), 'missing': [],
               'reasons': [], 'configuredDifferences': [], 'runningDifferences': []}
    if mode == 'runtime-different':
        runtime |= {'state': 'different', 'runningDifferences': ['Quality']}
        runtime['running']['Quality'] = '55'
    if mode == 'runtime-custom':
        runtime |= {'state': 'custom', 'custom': True}
    if mode == 'runtime-partial':
        runtime |= {'state': 'partial', 'runningVerified': False, 'missing': ['Quality'], 'reasons': ['missing-field']}
        del runtime['running']['Quality']
    if mode in ('runtime-unavailable', 'runtime-stale', 'runtime-denied'):
        state_name = mode.removeprefix('runtime-')
        runtime |= {'state': state_name, 'configuredVerified': False, 'runningVerified': False,
                    'configured': {}, 'running': {}, 'reasons': [state_name]}
    if mode == 'runtime-reload':
        runtime |= {'state': 'partial', 'needsReload': True, 'configuredVerified': False,
                    'configured': {}, 'reasons': ['manager-reload']}
    if mode == 'runtime-revision':
        runtime['storedRevision'] = 'c' * 64
    if mode == 'wrong-scope':
        runtime['scope'] = 'virtual' if scope == 'console' else 'console'
    if mode == 'private-field':
        runtime['argv'] = ['fixture-private-diagnostic']
    if mode == 'runtime-lie':
        runtime['pid'] = 0
    print(json.dumps({'runtime': runtime}))
    sys.exit(0)
if request['operation'] == 'save':
    assert set(request).issubset({'version', 'operation', 'scope', 'revision', 'values', 'tls'})
    if mode == 'stale' or request['revision'] != state['revision']:
        print(json.dumps({'error': 'host settings changed; reload before saving'}))
        sys.exit(1)
    state['values'] = request['values']
    tls = request.get('tls', {})
    if tls.get('mode') == 'import':
        assert 'Certificate' not in state['values'] and 'CertificateKey' not in state['values']
        assert 'PRIVATE KEY' in tls['privateKeyPem']
        # Store only fixture evidence of stdin transport, never the PEM.
        (base / 'import-seen').write_text('stdin only')
        state['values']['Certificate'] = '/etc/farside/tls-imports/fixture/certificate.crt'
        state['values']['CertificateKey'] = '/etc/farside/tls-imports/fixture/private.key'
    state['revision'] = hashlib.sha256(json.dumps(state['values'], sort_keys=True).encode()).hexdigest()
    state_file.write_text(json.dumps(state))
    if mode == 'crash-after-save':
        os.kill(os.getpid(), 9)
    if mode == 'saved-error':
        print(json.dumps({'saved': True, 'error': 'readback failed fixture-private-diagnostic'}))
        sys.exit(1)
snapshot = {'version': 1, 'scope': scope, 'revision': state['revision'], 'values': state['values'],
            'defaults': defaults, 'effective': defaults | state['values'], 'runtimeVerified': False,
            'application': 'new-desktops' if scope == 'session' else 'broker-restart'}
if scope == 'session':
    snapshot['renderDevices'] = [{'pci': '0000:01:00.0', 'driver': 'nvidia', 'render': '/dev/dri/renderD128'}]
else:
    snapshot['tls'] = {'state': 'valid', 'administratorManaged': False, 'fingerprint': ':'.join(['AA'] * 32),
                       'algorithm': 'ECDSA P-256', 'notBefore': '2026-09-01T00:00:00Z', 'notAfter': '2030-09-01T00:00:00Z'}
    snapshot['cameraLoopback'] = {'supported': scope == 'console', 'state': 'disabled'}
if scope != 'session' and mode in ('encoders', 'encoders-bad'):
    snapshot['videoEncoders'] = [
        {'codec': 'avc', 'backend': 'libx264', 'hw': False},
        {'codec': 'hevc', 'backend': 'nvenc', 'hw': True, 'device': '0000:09:00.0', 'name': 'NVIDIA GeForce RTX 2070'},
        {'codec': 'hevc', 'backend': 'libx265', 'hw': False}]
    if mode == 'encoders-bad':
        snapshot['videoEncoders'].append({'codec': 'vp9', 'backend': 'x', 'hw': False})
if mode == 'wrong-scope':
    snapshot['scope'] = 'session' if scope != 'session' else 'console'
if mode == 'private-field':
    snapshot['privateKeyPem'] = 'fixture-private-diagnostic'
if mode == 'runtime-lie':
    snapshot['runtimeVerified'] = True
if mode == 'wrong-effective':
    snapshot['effective'] = {}
reply = {'snapshot': snapshot}
if request['operation'] == 'save':
    reply |= {'saved': True, 'restartRequired': scope != 'session', 'newDesktopRequired': scope == 'session'}
print(json.dumps(reply))
