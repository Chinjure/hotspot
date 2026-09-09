#include "path_query.h"

#include <cwctype>

#include "file_rank.h"
#include "winutil.h"

namespace path_query {
namespace {

bool isSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// Split a normalized path into non-empty segments; separators collapse.
std::vector<std::wstring> splitSegments(const std::wstring& path) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == L'\\') {
            if (i > start) out.emplace_back(path, start, i - start);
            start = i + 1;
        }
    }
    return out;
}

// Advance `index` over the folded query segments that occur (substring) inside
// the path segment [text, text + len). One path segment may satisfy several
// query segments ("ab" satisfies both "a" and "b").
void advanceThroughSegment(const wchar_t* text, size_t len,
                           const std::vector<std::wstring>& folded, size_t& index) {
    while (index < folded.size() &&
           winutil::containsFolded(text, len, folded[index].data(), folded[index].size())) {
        index++;
    }
}

// True when `path` is the directory `foldedPrefix` itself or sits below it.
// `foldedPrefix` may or may not end with a separator.
bool isUnderPrefix(const std::wstring& path, const std::wstring& foldedPrefix) {
    if (foldedPrefix.empty()) return true;
    const bool prefixEndsWithSeparator = foldedPrefix.back() == L'\\';
    const size_t compared = std::min(path.size(), foldedPrefix.size());
    for (size_t i = 0; i < compared; i++) {
        if (winutil::foldChar(path[i]) != foldedPrefix[i]) return false;
    }
    if (path.size() == foldedPrefix.size()) return true;
    if (path.size() > foldedPrefix.size()) {
        return prefixEndsWithSeparator || path[foldedPrefix.size()] == L'\\';
    }
    // Shorter than the prefix: only the prefix directory without its trailing
    // separator counts ("C:\Users\me" is under "C:\Users\me\", "C:" is not).
    return prefixEndsWithSeparator && path.size() + 1 == foldedPrefix.size();
}

// Tier of a candidate whose own name matched the query's last segment.
Match tierForName(const std::wstring& name, const Query& query) {
    if (query.foldedSegments.empty()) return Match::None;
    if (!winutil::containsFolded(name, query.foldedSegments.back())) return Match::None;
    return static_cast<Match>(file_rank::matchScore(name, query.lastSegment()));
}

} // namespace

const std::wstring& Query::lastSegment() const {
    static const std::wstring kEmpty;
    return segments.empty() ? kEmpty : segments.back();
}

bool looksLikePath(const std::wstring& text) {
    if (text.find(L'\\') == std::wstring::npos && text.find(L'/') == std::wstring::npos) {
        return false;
    }
    // A lone separator is not a path query.
    for (wchar_t c : text) {
        if (!isSeparator(c) && !iswspace(c)) return true;
    }
    return false;
}

Query parse(const std::wstring& text) {
    Query query;
    std::wstring normalized;
    normalized.reserve(text.size());
    for (wchar_t c : text) normalized.push_back(isSeparator(c) ? L'\\' : c);

    size_t begin = 0;
    size_t end = normalized.size();
    while (begin < end && iswspace(normalized[begin])) begin++;
    while (end > begin && iswspace(normalized[end - 1])) end--;
    normalized = normalized.substr(begin, end - begin);

    query.trailingSeparator = !normalized.empty() && normalized.back() == L'\\';
    query.segments = splitSegments(normalized);
    query.valid = !query.segments.empty();

    for (size_t i = 0; i < query.segments.size(); i++) {
        if (i) query.normalized.push_back(L'\\');
        query.normalized += query.segments[i];
    }
    if (query.trailingSeparator && !query.normalized.empty()) query.normalized.push_back(L'\\');

    query.foldedSegments.reserve(query.segments.size());
    for (const auto& segment : query.segments) {
        query.foldedSegments.push_back(winutil::foldString(segment));
    }
    // Everything before the final segment names a directory; the final segment
    // names the target itself (a file, or the folder when the query ends with a
    // separator).
    query.leadingCount = query.segments.empty() ? 0 : query.segments.size() - 1;

    std::wstring leadPrefix;
    for (size_t i = 0; i + 1 < query.segments.size(); i++) {
        leadPrefix += query.segments[i];
        leadPrefix += L'\\';
    }
    query.foldedLeadPrefix = winutil::foldString(leadPrefix);
    query.foldedDirPrefix =
        query.segments.empty() ? std::wstring() : winutil::foldString(leadPrefix + query.segments.back());

    if (!query.segments.empty() && query.segments[0].size() == 2 && query.segments[0][1] == L':' &&
        iswalpha(query.segments[0][0])) {
        query.absolute = true;
        query.driveRoot = query.segments[0] + L"\\";
    }
    return query;
}

Match scorePath(const std::wstring& parentPath, const std::wstring& name, bool isDir,
                const Query& query) {
    if (!query.valid) return Match::None;

    if (query.absolute) {
        // Literal scope: the given segments must appear verbatim in the path, so
        // "C:\Windows\" cannot match a folder that merely contains "Windows".
        if (!query.trailingSeparator) {
            if (!isUnderPrefix(parentPath, query.foldedLeadPrefix)) return Match::None;
            return tierForName(name, query);
        }
        // "X:\dir\": the folder itself, or anything inside it.
        if (isDir && winutil::equalsIgnoreCase(name, query.lastSegment()) &&
            isUnderPrefix(parentPath, query.foldedLeadPrefix)) {
            return Match::NameExact;
        }
        return isUnderPrefix(parentPath, query.foldedDirPrefix) ? Match::Descendant : Match::None;
    }

    // Relative query: ordered segment matching anywhere in the path.
    // Count how many query segments occur, in order, inside the ancestor
    // segments of the candidate (one ancestor segment may satisfy several
    // query segments).
    size_t index = 0;
    const size_t total = query.foldedSegments.size();
    const size_t leading = query.leadingCount;

    size_t start = 0;
    for (size_t i = 0; i <= parentPath.size() && index < total; i++) {
        if (i == parentPath.size() || parentPath[i] == L'\\') {
            if (i > start) {
                advanceThroughSegment(parentPath.data() + start, i - start, query.foldedSegments, index);
            }
            start = i + 1;
        }
    }

    if (!query.trailingSeparator) {
        if (index < leading) return Match::None;
        return tierForName(name, query);
    }

    // Trailing separator: the candidate is the named folder itself (its own
    // name satisfies the final segment) or lives inside it (every segment was
    // matched by ancestor directories). A file whose name merely contains the
    // final segment is neither.
    if (index < leading) return Match::None;
    if (isDir) {
        Match tier = tierForName(name, query);
        if (tier != Match::None) return tier;
    }
    if (index >= total) return Match::Descendant;
    return Match::None;
}

std::wstring existingRootOf(const Query& query) {
    if (!query.absolute || query.segments.empty()) return L"";
    // Longest literal directory prefix of the query that exists on disk; the
    // final segment names the target, so it is only included when the query
    // itself ends with a separator.
    size_t count = query.trailingSeparator ? query.segments.size() : query.segments.size() - 1;
    std::wstring candidate;
    for (size_t i = 0; i < count; i++) {
        candidate = i == 0 ? query.segments[0] + L"\\" : winutil::joinPath(candidate, query.segments[i]);
    }
    while (!candidate.empty()) {
        if (winutil::dirExists(candidate)) return candidate;
        std::wstring parent = winutil::parentDir(candidate);
        if (parent.size() == 2 && parent[1] == L':') {
            parent += L"\\";
            if (winutil::dirExists(parent)) return parent;
            break;
        }
        if (parent == candidate || parent.empty()) break;
        candidate = parent;
    }
    return L"";
}

} // namespace path_query
