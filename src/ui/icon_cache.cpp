#include "icon_cache.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <cwctype>

#include "winutil.h"

namespace {

constexpr size_t kMaxCachedIcons = 768;

std::wstring lowerKey(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

bool isShortcutExt(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".lnk" || ext == L".url";
}

// Cache key: exe/lnk need a per-file icon (each app has its own logo); every
// other file type shares one icon per extension, like Explorer's icon cache.
std::wstring cacheKeyFor(const std::wstring& path) {
    std::wstring key = lowerKey(path);
    if (isShortcutExt(key) || (key.size() >= 4 && key.compare(key.size() - 4, 4, L".exe") == 0)) {
        return key;
    }
    size_t slash = key.find_last_of(L"\\/");
    size_t dot = key.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return key;
    return L"ext:" + key.substr(dot);
}

// The shell image lists are process-wide singletons; resolve each size once.
IImageList* shellImageList(int shil) {
    IImageList* il = nullptr;
    if (FAILED(SHGetImageList(shil, IID_IImageList, reinterpret_cast<void**>(&il)))) return nullptr;
    return il;
}

// Real icon for a path: shell image list first (jumbo -> extra large -> large),
// then the classic 32x32 SHGetFileInfo icon. Both routes go through the shell's
// own icon cache, which is exactly how Everything/Explorer resolve icons.
HICON shellIconForPath(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return nullptr;

    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(path.c_str(), attrs, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX)) return nullptr;

    const int sizes[] = {SHIL_JUMBO, SHIL_EXTRALARGE, SHIL_LARGE};
    for (int shil : sizes) {
        IImageList* il = shellImageList(shil);
        if (!il) continue;
        HICON icon = nullptr;
        HRESULT hr = il->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon);
        if (SUCCEEDED(hr) && icon) return icon;
    }

    SHFILEINFOW fallback{};
    if (SHGetFileInfoW(path.c_str(), attrs, &fallback, sizeof(fallback), SHGFI_ICON | SHGFI_LARGEICON) &&
        fallback.hIcon) {
        return fallback.hIcon;
    }
    return nullptr;
}

// Shortcuts point at another file; when the shell returns nothing for the .lnk
// itself (broken/stale target, missing icon location), follow the target.
std::wstring resolveShortcutTarget(const std::wstring& path) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return L"";
    }
    std::wstring target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
        if (SUCCEEDED(file->Load(path.c_str(), STGM_READ))) {
            wchar_t buffer[MAX_PATH * 2] = {};
            WIN32_FIND_DATAW data{};
            if (SUCCEEDED(link->GetPath(buffer, static_cast<int>(std::size(buffer)), &data, SLGP_RAWPATH))) {
                target = buffer;
            }
        }
        file->Release();
    }
    link->Release();
    return target;
}

// Render a HICON into a premultiplied-ARGB WIC bitmap.
//
// DrawIconEx into a top-down 32bpp DIB handles both icon layouts at once: icons
// with a real alpha channel come out correct, mask-based icons come out opaque
// with the mask holes filled black. The mask (ICONINFO.hbmMask) is then used to
// clear those holes so the row background shows through. This avoids the
// CreateBitmapFromHBITMAP/CreateBitmapFromWicBitmap path, which fails or loses
// alpha for several shell icon formats.
bool renderIconToArgb(IWICImagingFactory* wic, HICON icon, const ICONINFO& info, IWICBitmap** out) {
    if (!wic || !icon || !out) return false;

    int w = GetSystemMetrics(SM_CXICON);
    int h = GetSystemMetrics(SM_CYICON);
    if (info.hbmColor) {
        BITMAP bm{};
        if (GetObject(info.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
            w = bm.bmWidth;
            h = bm.bmHeight;
        }
    }
    if (w <= 0 || h <= 0) return false;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) return false;

    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    memset(bits, 0, bytes);

    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, dib);
    BOOL drawn = DrawIconEx(mem, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL);
    SelectObject(mem, old);
    DeleteDC(mem);
    if (!drawn) {
        DeleteObject(dib);
        return false;
    }

    // Any alpha at all means the icon carried its own alpha channel.
    BYTE* pixels = static_cast<BYTE*>(bits);
    bool hasAlpha = false;
    for (size_t i = 3; i < bytes; i += 4) {
        if (pixels[i] != 0) { hasAlpha = true; break; }
    }

    if (!hasAlpha && info.hbmMask) {
        // Mask bits are white for transparent pixels.
        BITMAP maskBm{};
        int maskW = w, maskH = h;
        if (GetObject(info.hbmMask, sizeof(maskBm), &maskBm) && maskBm.bmWidth > 0) {
            maskW = maskBm.bmWidth;
            maskH = maskBm.bmHeight;
        }
        HDC maskDc = CreateCompatibleDC(nullptr);
        HGDIOBJ oldMask = SelectObject(maskDc, info.hbmMask);
        for (int y = 0; y < h; y++) {
            int my = (maskH == h * 2) ? y : y; // 1bpp AND+OR masks are double height
            if (my >= maskH) break;
            for (int x = 0; x < w && x < maskW; x++) {
                COLORREF m = GetPixel(maskDc, x, my);
                bool transparent = (m != CLR_INVALID) && (GetRValue(m) + GetGValue(m) + GetBValue(m) > 0x80);
                pixels[(static_cast<size_t>(y) * w + x) * 4 + 3] = transparent ? 0 : 255;
            }
        }
        SelectObject(maskDc, oldMask);
        DeleteDC(maskDc);
    } else if (hasAlpha) {
        // Already premultiplied by DrawIconEx.
    } else {
        for (size_t i = 3; i < bytes; i += 4) pixels[i] = 255;
    }

    HRESULT hr = wic->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h),
                                             GUID_WICPixelFormat32bppPBGRA,
                                             static_cast<UINT>(w) * 4, static_cast<UINT>(bytes),
                                             pixels, out);
    DeleteObject(dib);
    return SUCCEEDED(hr) && *out != nullptr;
}

} // namespace

IconCache::IconCache(ID2D1RenderTarget* target) : target_(target) {
    InitializeCriticalSection(&cs_);
}

IconCache::~IconCache() {
    clear();
    if (wic_) wic_->Release();
    wic_ = nullptr;
    DeleteCriticalSection(&cs_);
}

bool IconCache::ensureWic() {
    if (wic_) return true;
    if (wicFailed_) return false;
    // WIC needs COM on this thread; the app calls OleInitialize at startup.
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic_));
    if (FAILED(hr) || !wic_) {
        wicFailed_ = true;
        winutil::debugLog(L"icon_cache: WIC factory unavailable hr=" + std::to_wstring(static_cast<unsigned>(hr)));
        return false;
    }
    return true;
}

ID2D1Bitmap* IconCache::loadLocked(const std::wstring& path) {
    if (!target_ || path.empty() || !ensureWic()) return nullptr;

    HICON icon = shellIconForPath(path);
    if (!icon && isShortcutExt(path)) {
        std::wstring target = resolveShortcutTarget(path);
        if (!target.empty()) icon = shellIconForPath(target);
    }
    if (!icon) return nullptr;

    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) {
        DestroyIcon(icon);
        return nullptr;
    }

    IWICBitmap* wicBitmap = nullptr;
    bool ok = renderIconToArgb(wic_, icon, info, &wicBitmap);
    DestroyIcon(icon);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (!ok || !wicBitmap) return nullptr;

    ID2D1Bitmap* bitmap = nullptr;
    HRESULT hr = target_->CreateBitmapFromWicBitmap(wicBitmap, nullptr, &bitmap);
    wicBitmap->Release();
    if (FAILED(hr) || !bitmap) return nullptr;
    return bitmap;
}

ID2D1Bitmap* IconCache::get(const std::wstring& path) {
    if (path.empty()) return nullptr;
    std::wstring key = cacheKeyFor(path);

    EnterCriticalSection(&cs_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        ID2D1Bitmap* cached = it->second;
        LeaveCriticalSection(&cs_);
        return cached;
    }
    if (cache_.size() >= kMaxCachedIcons) {
        for (auto& [k, v] : cache_) {
            if (v) v->Release();
        }
        cache_.clear();
    }
    ID2D1Bitmap* bitmap = loadLocked(path);
    if (bitmap) cache_.emplace(std::move(key), bitmap);
    LeaveCriticalSection(&cs_);
    return bitmap;
}

void IconCache::clear() {
    EnterCriticalSection(&cs_);
    for (auto& [key, bitmap] : cache_) {
        if (bitmap) bitmap->Release();
    }
    cache_.clear();
    LeaveCriticalSection(&cs_);
}

size_t IconCache::size() const {
    EnterCriticalSection(&cs_);
    size_t n = cache_.size();
    LeaveCriticalSection(&cs_);
    return n;
}
