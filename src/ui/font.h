// Warren -- the editor's typeface.
//
// A 5x7 dot matrix, embedded as five columns of seven bits per
// glyph. An engine that needs a font file before it can print
// "loading" has a bootstrapping problem, and a UI that cannot say
// anything until an asset pipeline works is a UI that cannot report
// what went wrong with the asset pipeline.
//
// Five by seven because that is the smallest grid on which every
// ASCII letter is still distinguishable -- 'B' from '8', 'l' from
// '1', 'O' from '0' -- and because at one device pixel per dot it is
// sharp, which a resampled TrueType at 11 px is not.
#pragma once

#include <cstdint>

namespace wr::ui {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphAdvance = 6;    // one column of space between letters
constexpr int kLineHeight = 10;
constexpr int kFirstGlyph = 32;     // space
constexpr int kLastGlyph = 126;     // tilde

// Column-major: bit 0 is the top row. Out of range returns the box
// that says "this glyph is missing", which is more useful than
// nothing at all.
const uint8_t *glyph_columns(int codepoint);

}  // namespace wr::ui
