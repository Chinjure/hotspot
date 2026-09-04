#pragma once

#include <string>

namespace app_paths {

// %LOCALAPPDATA%\hotspot (with one-time migration from the old
// %LOCALAPPDATA%\PowerToysRunStandalone directory).
std::wstring dataDir();

} // namespace app_paths
