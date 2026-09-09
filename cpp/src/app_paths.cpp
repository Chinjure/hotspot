#include "app_paths.h"

#include <windows.h>

#include "winutil.h"

namespace app_paths {

std::wstring dataDir() {
    std::wstring root = winutil::localAppData();
    std::wstring newDir = winutil::joinPath(root, L"hotspot");
    std::wstring oldDir = winutil::joinPath(root, L"PowerToysRunStandalone");

    if (!winutil::dirExists(newDir) && winutil::dirExists(oldDir)) {
        // MoveFileExW can rename the whole directory tree.
        if (MoveFileExW(oldDir.c_str(), newDir.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return newDir;
        }
        return oldDir;
    }
    return newDir;
}

} // namespace app_paths
