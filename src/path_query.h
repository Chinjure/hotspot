#pragma once

#include <string>
#include <vector>

// Path-mode file search: enabled as soon as the query contains a path separator
// ("\" as specified, "/" accepted as the same thing), so "C:\Users\me\Desktop"
// or "src\components" filter by location instead of by file name alone.
//
// Matching rules (case-insensitive, invariant-locale folding):
//   * The query is split on separators into non-empty segments.
//   * Absolute query "X:\dir\...\name": the candidate must live under the
//     literal directory prefix (no skipping of the given segments) and its own
//     name must match the final segment (exact > exact stem > prefix > contains).
//     "X:\dir\..." (trailing separator) lists that folder and its whole subtree,
//     the folder itself ranking as an exact name match.
//   * Relative query "dir\...\name": the leading segments may match any ancestor
//     directories in order, so intermediate directories can be omitted
//     ("proj\main.cpp" matches "C:\work\proj\src\main.cpp"). A relative query
//     ending with a separator finds every folder whose own name matches the last
//     segment plus everything inside those folders.
//   * A plain file never satisfies a trailing-separator query by having the last
//     segment in its own name, so "C:\Windows\" cannot surface a stray
//     "windows-something.lnk" from an unrelated directory.
namespace path_query {

// Hit quality; None means "does not match". Smaller is better.
enum class Match : int {
    None = -1,
    NameExact = 0,      // name == last segment
    StemExact = 1,      // "name.ext" stem == last segment
    NamePrefix = 2,     // name starts with the last segment
    NameContains = 3,   // name contains the last segment
    Descendant = 4,     // hit lives inside the folder named by the query
};

struct Query {
    bool valid = false;             // at least one non-empty segment
    bool trailingSeparator = false; // query ends with a separator
    bool absolute = false;          // starts with "X:\"
    std::wstring normalized;        // separators unified, empty segments dropped
    std::wstring driveRoot;         // "C:\" when absolute, else empty
    std::vector<std::wstring> segments;       // original case
    std::vector<std::wstring> foldedSegments; // winutil::foldString of segments
    size_t leadingCount = 0;        // segments before the final (name) segment
    // Folded literal prefixes used by absolute queries:
    std::wstring foldedLeadPrefix;  // "X:\dir\...\" (all but the last segment)
    std::wstring foldedDirPrefix;   // "X:\dir\...\last" (no trailing separator)

    const std::wstring& lastSegment() const;
};

// True when `text` contains a separator and yields at least one segment.
bool looksLikePath(const std::wstring& text);

Query parse(const std::wstring& text);

// Match score for a candidate given the path of its containing directory, its
// own name and whether it is a directory. Paths are compared segment by segment,
// so intermediate directories may be omitted ("proj\main.cpp" matches
// "C:\work\proj\src\main.cpp").
Match scorePath(const std::wstring& parentPath, const std::wstring& name, bool isDir,
                const Query& query);

// Deepest existing directory prefix of an absolute query ("" when none exists),
// used to start a time-boxed live scan next to the target instead of at "C:\".
std::wstring existingRootOf(const Query& query);

} // namespace path_query
