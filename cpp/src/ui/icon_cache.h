#pragma once

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>

#include <string>
#include <unordered_map>

// Resolves the real Windows shell icon (exe logo, .lnk overlay, folder, file
// type) for a path and caches it as a Direct2D bitmap. Emoji fallback glyphs
// are only used when no icon can be resolved.
//
// Thread safety: the cache is mutex-guarded, but Direct2D bitmaps are device
// resources, so every load must happen on the UI thread while the render target
// is alive. clear() must be called whenever the render target is recreated.
class IconCache {
public:
    explicit IconCache(ID2D1RenderTarget* target);
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    // Cached bitmap for a file-system path, or nullptr when unavailable.
    ID2D1Bitmap* get(const std::wstring& path);

    void clear();
    size_t size() const;

private:
    ID2D1Bitmap* loadLocked(const std::wstring& path);
    bool ensureWic();

    ID2D1RenderTarget* target_ = nullptr; // owned by MainWindow
    IWICImagingFactory* wic_ = nullptr;
    bool wicFailed_ = false;
    std::unordered_map<std::wstring, ID2D1Bitmap*> cache_;
    mutable CRITICAL_SECTION cs_{};
};
