// Warren -- answering a question that was asked wrong.
//
// Somebody types "colour" where the engine says "color", or
// "OmniLight" where it says "OmniLight3D". The name is wrong, the
// intention is obvious, and "no such property" tells them only that
// their next guess should be different. Given the list of names that
// WOULD have worked, this picks the ones worth offering back.
//
// It matters more than it looks. The agent interface, the script
// bindings and the editor all take names from somebody who is
// guessing, and being right on the second try instead of the fifth
// is most of what makes a system pleasant to drive.
#pragma once

#include <string>
#include <vector>

namespace wr {

// Closest first, at most `limit`. Case-insensitive, and a candidate
// that contains the wanted string (or vice versa) always qualifies --
// somebody who wrote "position" at "global_position" knew roughly
// what they wanted.
std::vector<std::string> suggest(const std::string &wanted,
                                 const std::vector<std::string> &candidates,
                                 size_t limit = 5);

// Levenshtein distance, case-insensitive.
int edit_distance(const std::string &a, const std::string &b);

}  // namespace wr
