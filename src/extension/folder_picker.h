#pragma once

#include <string>
#include <windows.h>

namespace folder_picker {

// Modal "Browse For Folder" dialog. Returns the chosen folder path,
// or an empty string if the user cancelled.
// `title` is shown above the tree view.
// `initialDir` (optional) preselects the starting folder.
// `owner` is the dialog parent (may be nullptr).
std::string BrowseForFolder(HWND owner, const std::string& title, const std::string& initialDir);

}  // namespace folder_picker
