#pragma once

#include <windows.h>

namespace CandidateWindowGeometry {

// Scale relative to a desktop-space origin; never scale absolute virtual-screen coordinates.
inline LONG ScaleFromOrigin(LONG coordinate, LONG origin, UINT sourceDpi, UINT targetDpi) {
    if (!sourceDpi || !targetDpi || sourceDpi == targetDpi) return coordinate;
    return origin + MulDiv(coordinate - origin, static_cast<int>(targetDpi), static_cast<int>(sourceDpi));
}

inline RECT ScaleRectFromOrigin(const RECT& rect, const POINT& origin, UINT sourceDpi, UINT targetDpi) {
    return RECT{
        ScaleFromOrigin(rect.left, origin.x, sourceDpi, targetDpi),
        ScaleFromOrigin(rect.top, origin.y, sourceDpi, targetDpi),
        ScaleFromOrigin(rect.right, origin.x, sourceDpi, targetDpi),
        ScaleFromOrigin(rect.bottom, origin.y, sourceDpi, targetDpi),
    };
}

} // namespace CandidateWindowGeometry