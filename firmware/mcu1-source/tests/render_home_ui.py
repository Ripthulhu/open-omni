"""Make a review sheet from PBMs emitted by the real C test_home_ui binary.

Compile/run that binary with an output directory first. Pillow is needed only
for this optional host-side contact sheet, never by firmware or the C tests.
"""
import argparse
import hashlib
import json
from pathlib import Path


def render(directory, output):
    from PIL import Image, ImageDraw, ImageOps
    files = sorted(directory.glob('*.pbm'))
    if not files:
        raise ValueError('Run test_home_ui OUTPUT_DIRECTORY to produce real renderer PBMs first')
    width, height = 128 * 4, 64 * 4
    rows = (len(files) + 1) // 2
    sheet = Image.new('RGB', (width * 2 + 48, rows * (height + 52)), (12, 17, 23))
    draw = ImageDraw.Draw(sheet)
    manifest = {}
    for i, file in enumerate(files):
        with Image.open(file) as image:
            assert image.size == (128, 64), file
            # PBM encodes lit pixels as black. The real panel emits cyan.
            image = ImageOps.invert(image.convert('L'))
            image = ImageOps.colorize(image, (4, 16, 22), (32, 228, 244))
            image = image.resize((width, height), Image.Resampling.NEAREST)
        x, y = 16 + (i % 2) * (width + 16), 28 + (i // 2) * (height + 52)
        draw.text((x, y - 18), file.stem, fill=(210, 225, 234))
        sheet.paste(image, (x, y))
        manifest[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
    sheet.save(output)
    output.with_suffix('.json').write_text(json.dumps(dict(
        source='test_home_ui C binary / home_ui.c', size=[128, 64],
        scale='4x nearest-neighbor; cyan color for review only', fixtures=manifest),
        indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    render(args.directory, args.output)
