#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace CandidateWindow {

// Draws the candidate list under the caret. The caret is in screen coordinates.
// DPI is supplied by the foreground application.
bool Show(const std::vector<std::wstring>& candidates, UINT selection, RECT caret, UINT dpi);
// Standalone probe compatibility overload.
bool Show(const std::vector<std::wstring>& candidates, UINT selection, RECT caret);

void Hide();
void Destroy();
bool IsVisible();

} // namespace CandidateWindow
