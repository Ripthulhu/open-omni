"""Create an isolated HID environment, then run the flasher."""
from pathlib import Path
import os
import subprocess
import sys
import venv

here = Path(__file__).resolve().parent
environment = Path.home() / '.open-omni' / 'flasher-venv'
python = environment / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
if not python.exists():
    venv.EnvBuilder(with_pip=True).create(environment)
subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check',
                '-r', str(here / 'requirements.txt')], check=True)
result = subprocess.run([str(python), str(here / 'flash.py'), *sys.argv[1:]])
if not sys.argv[1:] and sys.stdin.isatty():
    input('Press Enter to close.')
sys.exit(result.returncode)
