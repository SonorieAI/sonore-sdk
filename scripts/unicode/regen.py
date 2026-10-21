# SPDX-License-Identifier: Apache-2.0
"""Regenerates the Unicode tables in gfx/bidi.h and gfx/text_shaper.h.

    python regen.py <ucd-directory>

The directory holds ArabicShaping.txt, BidiBrackets.txt, BidiMirroring.txt
and Scripts.txt from https://www.unicode.org/Public/<version>/ucd/ -- the
version of Python's own unicodedata module (python -c "import unicodedata;
print(unicodedata.unidata_version)"), which supplies the bidi classes,
general categories, combining classes and decompositions. fontTools gives
the scripts' ISO 15924 codes.

Each table sits between // <generated name="..."> and // </generated> in its
header; only what is between the markers is rewritten.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GFX = os.path.join(HERE, '..', '..', 'sdk', 'include', 'sonore', 'gfx')


def generate(script, ucd):
    env = dict(os.environ, SONORE_UCD=ucd)
    return subprocess.run([sys.executable, os.path.join(HERE, script)], capture_output=True,
                          text=True, check=True, env=env).stdout.rstrip('\n')


def splice(header, name, body):
    path = os.path.join(GFX, header)
    text = open(path, encoding='utf-8').read()
    start = text.index('// <generated name="%s">' % name)
    start = text.index('\n', start) + 1
    end = text.index('// </generated>', start)
    text = text[:start] + body + '\n' + text[end:]
    open(path, 'w', encoding='utf-8', newline='\n').write(text)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    ucd = os.path.abspath(sys.argv[1])
    splice('bidi.h', 'bidi', generate('gen_bidi.py', ucd))
    splice('line_break.h', 'linebreak', generate('gen_linebreak.py', ucd))
    splice('text_shaper.h', 'shaping', generate('gen_shaping.py', ucd))
    splice('text_shaper.h', 'compose', generate('gen_compose.py', ucd))
    splice('text_shaper.h', 'indic', generate('gen_indic.py', ucd))
    splice('text_shaper.h', 'use', generate('gen_use.py', ucd))
    splice('text_shaper.h', 'syllables', generate('gen_syllables.py', ucd))
    print('tables regenerated')


main()
