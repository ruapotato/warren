#include "core/suggest.h"

#include <algorithm>
#include <cctype>

namespace wr {

int edit_distance(const std::string &a, const std::string &b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = int(j);
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = int(i);
        for (size_t j = 1; j <= b.size(); j++) {
            const int cost =
                std::tolower(a[i - 1]) == std::tolower(b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[b.size()];
}

std::vector<std::string> suggest(const std::string &wanted,
                                 const std::vector<std::string> &candidates,
                                 size_t limit) {
    struct Scored {
        int d;
        std::string name;
    };
    std::vector<Scored> scored;
    // A third of the length, so a short name needs a close match and
    // a long one can afford a typo or two.
    const int budget = std::max(2, int(wanted.size()) / 3 + 1);
    for (const std::string &n : candidates) {
        const int d = edit_distance(wanted, n);
        const bool contains = !wanted.empty() &&
                              (n.find(wanted) != std::string::npos ||
                               wanted.find(n) != std::string::npos);
        if (d <= budget || contains) scored.push_back({contains ? std::min(d, 1) : d, n});
    }
    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        return a.d != b.d ? a.d < b.d : a.name < b.name;
    });
    std::vector<std::string> out;
    for (size_t i = 0; i < scored.size() && i < limit; i++)
        out.push_back(scored[i].name);
    return out;
}

}  // namespace wr
