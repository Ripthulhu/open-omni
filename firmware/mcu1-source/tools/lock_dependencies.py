"""Record exact existing checkouts; run in WSL before building."""
import json
import pathlib
import subprocess
import sys

vendor = pathlib.Path(sys.argv[1]).resolve()
rows = []
for name in ('devices', 'usb', 'core', 'examples', 'cmsis', 'legacy-sdk'):
    repo = vendor / name
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()
    if git('status', '--porcelain'):
        raise SystemExit(f'Dirty dependency: {name}')
    rows.append({'name': name, 'url': git('remote', 'get-url', 'origin'),
                 'revision': git('rev-parse', 'HEAD')})
out = pathlib.Path(__file__).resolve().parents[1] / 'dependencies.lock.json'
out.write_text(json.dumps(rows, indent=2) + '\n')
print(out)
