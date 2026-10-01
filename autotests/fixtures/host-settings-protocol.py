# Test-only subprocess fixture. Never reads or writes installed host settings.
import json
import os
import pathlib
import sys
import time

base = pathlib.Path(sys.argv[1])
mode = (base / 'mode').read_text().strip() if (base / 'mode').exists() else 'success'
request = json.load(sys.stdin)
assert all('PRIVATE KEY' not in value for value in sys.argv)
scope = request['scope']
assert scope in ('console', 'virtual', 'session')
defaults = {'RenderPci': '', 'VaapiDriver': 'auto'} if scope == 'session' else {
    'Address': '0.0.0.0', 'Port': '3391' if scope == 'console' else '3395',
    'Certificate': '/etc/farside/console.crt' if scope == 'console' else '/etc/farside/virtual-host.crt',
    'CertificateKey': '/etc/farside/console.key' if scope == 'console' else '/etc/farside/virtual-host.key',
    'Quality': '80', 'AdaptiveQuality': 'false', 'PreferAudioQuality': 'false',
    'StandardClientMedia': 'true', 'CameraLoopbackDevice': 'none', 'SoftwareEncoding': 'auto', 'Av1Tiles': 'auto'}
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
    state['revision'] = 'b' * 64
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
