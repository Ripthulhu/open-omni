"""Fetch the exact public SDK revisions used by the firmware."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def fetch(vendor):
    vendor.mkdir(parents=True, exist_ok=True)
    for dependency in json.loads((ROOT / 'dependencies.lock.json').read_text()):
        repo = vendor / dependency['name']
        if not repo.exists():
            subprocess.run(['git', 'init', str(repo)], check=True)
            subprocess.run(['git', '-C', str(repo), 'remote', 'add', 'origin', dependency['url']], check=True)
        if subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain']).strip():
            raise RuntimeError(f'Dependency has local changes: {repo}')
        subprocess.run(['git', '-C', str(repo), 'fetch', '--depth=1', dependency['url'], dependency['revision']], check=True)
        subprocess.run(['git', '-C', str(repo), 'checkout', '--detach', dependency['revision']], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vendor', type=Path)
    fetch(parser.parse_args().vendor.resolve())
