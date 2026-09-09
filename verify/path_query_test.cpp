// Unit tests for the path-search matcher (path_query::scorePath) and the
// path-mode ordering (file_rank::betterPathMatch).
//
// Build + run:  powershell -ExecutionPolicy Bypass -File verify\path-query-test.ps1
//
// These are pure logic tests over synthetic paths, so they run without an NTFS
// index, without touching the file system and in a fraction of a second.
#include <cstdio>
#include <string>

#include "../src/file_rank.h"
#include "../src/path_query.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        failures++;
    } else {
        std::printf("ok    %s\n", what);
    }
}

void checkTier(const wchar_t* parent, const wchar_t* name, bool isDir, const wchar_t* query,
               path_query::Match expected, const char* what) {
    path_query::Query q = path_query::parse(query);
    path_query::Match actual = path_query::scorePath(parent, name, isDir, q);
    if (actual != expected) {
        std::printf("FAIL  %s (expected %d, got %d) query=%ls parent=%ls name=%ls\n", what,
                    static_cast<int>(expected), static_cast<int>(actual), query, parent, name);
        failures++;
    } else {
        std::printf("ok    %s\n", what);
    }
}

constexpr int kNone = static_cast<int>(path_query::Match::None);
constexpr int kExact = static_cast<int>(path_query::Match::NameExact);
constexpr int kStem = static_cast<int>(path_query::Match::StemExact);
constexpr int kPrefix = static_cast<int>(path_query::Match::NamePrefix);
constexpr int kContains = static_cast<int>(path_query::Match::NameContains);
constexpr int kDescendant = static_cast<int>(path_query::Match::Descendant);

void testParsing() {
    check(path_query::looksLikePath(L"C:\\Windows"), "looksLikePath: absolute");
    check(path_query::looksLikePath(L"src\\components"), "looksLikePath: relative");
    check(path_query::looksLikePath(L"src/components"), "looksLikePath: forward slash");
    check(!path_query::looksLikePath(L"report.docx"), "looksLikePath: plain name");
    check(!path_query::looksLikePath(L"\\"), "looksLikePath: lone separator");

    path_query::Query q = path_query::parse(L"C:/Users/mayn/");
    check(q.valid, "parse: valid");
    check(q.absolute, "parse: absolute");
    check(q.trailingSeparator, "parse: trailing separator");
    check(q.normalized == L"C:\\Users\\mayn\\", "parse: normalized separators");
    check(q.segments.size() == 3, "parse: three segments");
    check(q.driveRoot == L"C:\\", "parse: drive root");

    path_query::Query rel = path_query::parse(L"proj\\main.cpp");
    check(!rel.absolute && !rel.trailingSeparator, "parse: relative file query");
    check(rel.leadingCount == 1, "parse: one leading segment");
}

void testNameQueries() {
    checkTier(L"C:\\Users\\mayn\\Desktop", L"proj", false, L"C:\\Users\\mayn\\Desktop\\proj",
              path_query::Match::NameExact, "absolute: last segment matches name");
    checkTier(L"C:\\work\\proj\\src", L"main.cpp", false, L"proj\\main",
              path_query::Match::StemExact, "relative: intermediate directory may be omitted");
    checkTier(L"C:\\work\\proj\\src", L"main.cpp", false, L"proj\\main.cpp",
              path_query::Match::NameExact, "relative: exact name");
    checkTier(L"C:\\gamma\\target-dir", L"deep.md", false, L"alpha\\beta\\target",
              path_query::Match::None, "leading segments must all match ancestors");
    checkTier(L"C:\\a\\beta2\\src", L"x.txt", false, L"a\\b\\x",
              path_query::Match::StemExact, "substring segments match in order");
    checkTier(L"C:\\Users\\mayn", L"notes.txt", false, L"C:\\Windows\\notes",
              path_query::Match::None, "wrong drive directory does not match");
    checkTier(L"C:\\Users\\mayn\\Desktop\\proj\\src", L"main.cpp", false,
              L"C:\\Users\\mayn\\Desktop\\proj", path_query::Match::None,
              "absolute: deeper files are not hits when the name does not match");
    checkTier(L"C:\\Users\\mayn\\Desktop\\other", L"proj.md", false,
              L"C:\\Users\\mayn\\Desktop\\proj", path_query::Match::StemExact,
              "absolute: name match inside the given prefix is scoped, not literal");
    checkTier(L"C:\\Users\\mayn\\Desktop\\other", L"notes.txt", false,
              L"C:\\Users\\mayn\\Desktop\\proj", path_query::Match::None,
              "absolute: other names under the prefix are rejected");
}

void testFolderListing() {
    checkTier(L"C:\\", L"Windows", true, L"C:\\Windows\\", path_query::Match::NameExact,
              "trailing: the named folder itself");
    checkTier(L"C:\\Windows", L"notepad.exe", false, L"C:\\Windows\\",
              path_query::Match::Descendant, "trailing: child of the named folder");
    checkTier(L"C:\\Windows\\System32\\drivers", L"etc.txt", false, L"C:\\Windows\\",
              path_query::Match::Descendant, "trailing: deep descendant");
    checkTier(L"C:\\Users\\mayn\\undex-libraries", L"windows.cut.svg.lnk", false, L"C:\\Windows\\",
              path_query::Match::None, "trailing: unrelated file named windows* is rejected");
    checkTier(L"C:\\proj", L"src", true, L"src\\", path_query::Match::NameExact,
              "relative trailing: folder itself");
    checkTier(L"C:\\proj\\src", L"a.txt", false, L"src\\", path_query::Match::Descendant,
              "relative trailing: contents of the folder");
    checkTier(L"C:\\proj", L"srcfile.txt", false, L"src\\", path_query::Match::None,
              "relative trailing: file named src* is rejected");
    checkTier(L"C:\\Program Files (x86)\\Windows Media Player", L"wmprph.exe", false,
              L"C:\\Windows\\", path_query::Match::None,
              "absolute trailing: literal scope, no folder containing Windows");
    checkTier(L"C:\\WindowsApps", L"x.txt", false, L"C:\\Windows\\", path_query::Match::None,
              "absolute trailing: sibling folder with a shared prefix is rejected");
}

void testOrdering() {
    // Group order: exe/lnk first, then folders, then other files.
    check(file_rank::betterPathMatch(L"setup.exe", false, kDescendant, L"tools", true, kExact),
          "ordering: exe before folder even with a worse tier");
    check(file_rank::betterPathMatch(L"tools", true, kDescendant, L"readme.md", false, kExact),
          "ordering: folder before file even with a worse tier");
    check(file_rank::betterPathMatch(L"report.md", false, kExact, L"readme.md", false, kContains),
          "ordering: better tier first inside one group");
    check(file_rank::betterPathMatch(L"setup.exe", false, kExact, L"setup.lnk", false, kExact),
          "ordering: exe before lnk at the same tier");
    check(!file_rank::betterPathMatch(L"readme.md", false, kExact, L"tools", true, kExact),
          "ordering: file does not precede folder");

    // Name mode must keep the historical behaviour (files before folders).
    check(file_rank::betterMatch(L"report.md", false, L"reports", true, L"report"),
          "regression: name mode still prefers files over folders");
    check(file_rank::betterMatch(L"steam.exe", false, L"steam", true, L"steam"),
          "regression: name mode still prefers launchable");
}

} // namespace

int wmain() {
    testParsing();
    testNameQueries();
    testFolderListing();
    testOrdering();
    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
