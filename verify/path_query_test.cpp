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
    checkTier(L"C:\\ProgramData\\Microsoft", L"Windows", true, L"C:\\Windows\\",
              path_query::Match::None,
              "absolute trailing: a deeper folder with the same name is not the named folder");
    checkTier(L"C:\\a", L"b", true, L"C:\\a\\b\\", path_query::Match::NameExact,
              "absolute trailing: three-segment query names that exact folder");
    checkTier(L"C:\\other\\a", L"b", true, L"C:\\a\\b\\", path_query::Match::None,
              "absolute trailing: the same name deeper down is rejected");
    checkTier(L"C:\\Windows", L"System32", true, L"C:\\Windows\\", path_query::Match::Descendant,
              "absolute trailing: a child folder of the named folder is a descendant");
    checkTier(L"C:\\WindowsApps", L"x.txt", false, L"C:\\Windows\\", path_query::Match::None,
              "absolute trailing: sibling folder with a shared prefix is rejected");
}

// Shorthand for a name-mode comparison at the tiers matchScore() would give.
bool nameOrder(const wchar_t* a, bool aDir, const wchar_t* b, bool bDir, const wchar_t* query) {
    return file_rank::betterNameMatch(a, aDir, file_rank::matchScore(a, query),
                                      b, bDir, file_rank::matchScore(b, query));
}

void testOrdering() {
    // The match decides first: a better tier beats any kind preference.
    check(file_rank::betterPathMatch(L"tools", true, kExact, L"setup.exe", false, kDescendant),
          "ordering: folder named by the query before a loose exe inside it");
    check(file_rank::betterPathMatch(L"readme.md", false, kExact, L"tools", true, kDescendant),
          "ordering: exact-name file before a worse-tier folder");
    check(file_rank::betterPathMatch(L"report.md", false, kExact, L"readme.md", false, kContains),
          "ordering: better tier first inside one group");
    check(!file_rank::betterPathMatch(L"readme.md", false, kContains, L"tools", true, kExact),
          "ordering: kind does not outrank a worse path match");

    // Same path tier: exe/lnk, then folders, then other files.
    check(file_rank::betterPathMatch(L"setup.exe", false, kExact, L"tools", true, kExact),
          "ordering: exe before folder at the same tier");
    check(file_rank::betterPathMatch(L"tools", true, kExact, L"readme.md", false, kExact),
          "ordering: folder before file at the same tier");
    check(file_rank::betterPathMatch(L"setup.exe", false, kExact, L"setup.lnk", false, kExact),
          "ordering: exe before lnk at the same tier");
    check(!file_rank::betterPathMatch(L"readme.md", false, kExact, L"tools", true, kExact),
          "ordering: file does not precede folder");
    check(!file_rank::betterPathMatch(L"proj", true, kExact, L"proj.exe", false, kStem),
          "ordering: exe before the exact folder inside one quality class");
}

void testNameOrdering() {
    // Reported bug: a shortcut that merely shares the word must not outrank the
    // folder whose name is exactly the query.
    check(nameOrder(L"clock", true, L"Clock Widget.lnk", false, L"clock"),
          "name ordering: exact folder before prefix lnk");
    check(nameOrder(L"clock", true, L"clockify.lnk", false, L"clock"),
          "name ordering: exact folder before prefix lnk (no space)");
    check(nameOrder(L"clock", true, L"myclock.exe", false, L"clock"),
          "name ordering: exact folder before contains exe");
    check(nameOrder(L"clock", true, L"clocks", true, L"clock"),
          "name ordering: exact folder before prefix folder");

    // Equal name quality: the launcher kinds keep their order.
    check(nameOrder(L"steam.exe", false, L"steam", true, L"steam"),
          "name ordering: exe (exact stem) before the exact folder");
    check(nameOrder(L"Steam.lnk", false, L"steam", true, L"steam"),
          "name ordering: lnk before the exact folder");
    check(nameOrder(L"steam.exe", false, L"Steam.lnk", false, L"steam"),
          "name ordering: exe before lnk");
    check(nameOrder(L"clock", true, L"clock.txt", false, L"clock"),
          "name ordering: exact name before exact stem when the kinds tie");

    // A better match still beats a folder, which used to be the other way round.
    check(nameOrder(L"report.md", false, L"reports", true, L"report"),
          "name ordering: exact-stem file before prefix folder");
}

} // namespace

int wmain() {
    testParsing();
    testNameQueries();
    testFolderListing();
    testOrdering();
    testNameOrdering();
    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
