#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Candidate window owned by the TSF DLL.
//
// Most modern applications (Notepad, browsers, editors) never render the
// ITfCandidateListUIElement that a text service exposes, so the candidate list
// has to be drawn by the text service itself. The window lives on the thread
// that hosts the text service, never takes focus, and stays above the owner
// window.
namespace CandidateWindow {

// Called on the owning thread while the list is waiting for a usable caret
// rectangle. Applications lay out composition text asynchronously, so the
// rectangle is usually not available in the edit session that inserted it.
using CaretProvider = bool (*)(RECT& caret);

// Draws the candidate list under the caret. `caret` is in screen coordinates.
// An empty rectangle asks the caret provider to retry for a short while and
// falls back to the application window. An empty candidate list hides the
// window and returns false.
bool Show(const std::vector<std::wstring>& candidates, UINT selection, RECT caret);

// Registers the callback used to retry the caret lookup. Pass nullptr to stop
// retrying, for example while the text service is deactivated.
void SetCaretProvider(CaretProvider provider);

// Hides the window but keeps it alive for the next composition.
void Hide();

// Hides and destroys the window. Called when the text service deactivates.
void Destroy();

bool IsVisible();

} // namespace CandidateWindow