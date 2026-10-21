# Unicode tables

`gfx/bidi.h`, `gfx/text_shaper.h` and `gfx/line_break.h` carry tables generated
from the Unicode Character Database: bidi classes, paired brackets and
mirroring; line breaking classes (with East_Asian_Width and
Extended_Pictographic for two of the rules); Arabic joining
types; scripts; combining marks, combining classes and default ignorables; the
canonical decompositions and compositions; the Indic, Khmer and Myanmar
categories and positions. `regen.py` rewrites them:

```sh
python regen.py path/to/ucd     # needs fontTools; the UCD of Python's unicodedata version
```

The UCD directory holds ArabicShaping.txt, BidiBrackets.txt,
BidiMirroring.txt, Scripts.txt, IndicSyllabicCategory.txt,
IndicPositionalCategory.txt, Blocks.txt, DerivedCoreProperties.txt,
LineBreak.txt, EastAsianWidth.txt and emoji/emoji-data.txt.

Two generators read something other than the UCD:

- `gen_syllables.py` compiles HarfBuzz's syllable grammars -- Indic, Khmer,
  Myanmar, Universal, written out in it as HarfBuzz's Ragel files have them --
  to minimal state machines, longest match first, as Ragel builds them.
- `gen_indic.py` takes the categories from the UCD by HarfBuzz's
  gen-indic-table.py rules, and Microsoft's invalid vowel sequences from
  `ms-use/IndicShapingInvalidCluster.txt` (MIT, `ms-use/COPYING`).
- `gen_use.py` takes the Universal Shaping Engine's categories from the UCD
  and Microsoft's additions in `ms-use/` by HarfBuzz's gen-use-table.py
  rules -- for the characters this version of Unicode has: HarfBuzz builds
  from a newer one, and where Unicode has since changed a category (Batak's
  two killers, made reordering killers in 17.0) the tables here follow 15.0.

## Checked against Unicode's own conformance file

`tests/data/bidi/BidiCharacterTest-15.0.0-sample.txt` is every hundredth line
of Unicode's `BidiCharacterTest.txt`, and `sdk_tests` runs it. The whole file --
91,707 paragraphs, explicit embeddings, isolates, brackets and all -- passes:
build a probe that reads it the way `sdk_tests` reads the sample and point it at
the full file from the UCD.

## Line breaking, checked against Unicode's own test

`tests/data/linebreak/LineBreakTest-15.0.0-sample.txt` is every eighth case of
Unicode's `auxiliary/LineBreakTest.txt`, and `sdk_tests` runs it; all 7,654
cases of the full file pass. The test assumes UAX #14's tailoring for numbers
(its Example 7), and `line_break.h` implements that tailoring.

## Shaping, checked against HarfBuzz

`text_shaper.h` follows HarfBuzz's behaviour -- its feature stages, its
normalisation, its shapers -- and is compared with it glyph by glyph:
`tests/data/shaping/make_shaping.py` and `make_indic.py` write HarfBuzz's
output for fixture fonts beside the fonts, and `sdk_tests` wants the same.
Against the system fonts of a Windows machine, random strings come out as
HarfBuzz's: 336,000 Arabic, Hebrew, Latin, Greek and Cyrillic, 84,000 of them
again with joiners scattered through, 45,000 on fonts that cannot place their
marks, 31,500 Thai, Lao and Hangul, 27,000 Indic syllables across nine
scripts, 20,000 Khmer and Myanmar, and 48,000 syllables of twenty-one
Universal scripts -- all identical, but for Batak's two killers.

The Unicode data is © Unicode, Inc., under the Unicode License v3; HarfBuzz's
grammars and rules under its "Old MIT" licence; Microsoft's list under the MIT
licence. See THIRD_PARTY_NOTICES.md.
