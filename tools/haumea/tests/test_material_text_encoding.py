# SPDX-License-Identifier: GPL-2.0-or-later
"""Material text is decoded as UTF-8, so a Chinese character is one glyph
instead of one Windows-1252 box per byte.

The editor stores material text as UTF-8 (imported scripts are decoded to it
and the text field writes it), but the preview used to decode it one byte at a
time through Windows-1252: 你 (E4 BD A0) came out as three Latin-1 glyphs, and
choosing a font that has the CJK glyphs changed nothing, because the code points
asked for were the bytes.

Needs a built editor, a game folder, and a font the editor can resolve that has
CJK glyphs; name its family with HAUMEA_MATERIAL_CJK_FONT (KaiTi, SimHei,
Noto Sans CJK SC, ...). Without one the test is skipped: a machine with no CJK
glyphs can only draw .notdef boxes, which would prove nothing.
"""
from pathlib import Path
import os
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from haumea_test_env import editor_env, run_editor  # noqa: E402
from test_material_fonts import (  # noqa: E402
    BINARY, GAME_DIR, HAVE_DISPLAY, read_png_alpha)

CJK_FONT = os.environ.get('HAUMEA_MATERIAL_CJK_FONT')


@unittest.skipUnless(BINARY and GAME_DIR and HAVE_DISPLAY and CJK_FONT,
                     'set HAUMEA_GAME_DIR, build the editor and set '
                     'HAUMEA_MATERIAL_CJK_FONT to run')
class MaterialTextEncodingTests(unittest.TestCase):
    def private_env(self, tmp):
        env = editor_env(tmp)
        # No bundled font, so the family named above is resolved from the
        # system the way a player's machine would resolve it.
        empty = Path(tmp, 'no-fonts')
        empty.mkdir(exist_ok=True)
        env['HAUMEA_BUNDLED_FONTS_DIR'] = str(empty)
        return env

    def test_chinese_decodes_to_one_glyph_per_character(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp, 'cjk.png')
            # Material size 60 is the 128x128 texture; alignment 1 centres.
            result = run_editor([BINARY, '--game', GAME_DIR, '--material-text-png', out,
                                 '你好', '60', CJK_FONT, '90', '0', '1'],
                                cwd=tmp, env=self.private_env(tmp),
                                capture_output=True, text=True)
            self.assertIn('material-text-png: ' + str(out), result.stdout, result.stdout[-2000:])
            width, height, alpha = read_png_alpha(out)
            self.assertEqual((width, height), (128, 128))
            covered = sum(1 for row in alpha for a in row if a > 128)
            self.assertGreater(covered, 0, 'nothing was drawn')


if __name__ == '__main__':
    unittest.main()
