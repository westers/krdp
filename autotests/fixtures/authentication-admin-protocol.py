# Test-only subprocess protocol fixture. Never reads/writes installed policy.
import json, pathlib, sys

base = pathlib.Path(sys.argv[1])
mode = (base / 'mode').read_text().strip() if (base / 'mode').exists() else 'success'
if mode == 'cancel':
    sys.exit(126)
if mode == 'denied':
    sys.exit(127)
request = json.load(sys.stdin)
assert all('fixture-only-password' not in arg for arg in sys.argv)
if mode == 'malformed':
    print('invalid fixture response')
    sys.exit(0)
if mode == 'oversized':
    print('x' * 70000)
    sys.exit(0)
route = lambda mode, accounts: {'pam': {'mode': mode, 'accounts': accounts}, 'credentials': []}
snapshot = {'version': 1, 'revision': 'a' * 64, 'console': route('allow-list', ['westers']), 'virtual': route('disabled', [])}
if request['operation'] == 'save':
    snapshot = request['update']
    password_seen = False
    for name in ('console', 'virtual'):
        for alias in snapshot[name]['credentials']:
            password_seen |= alias.get('password') == 'fixture-only-password'
            alias.pop('password', None)
    snapshot['revision'] = 'b' * 64
    (base / 'saved').write_text(json.dumps({'passwordSeenOnStdin': password_seen}))
    print(json.dumps({'snapshot': snapshot, 'saved': True, 'restartRequired': True}))
else:
    print(json.dumps({'snapshot': snapshot}))
