﻿#include "precompiled.h"
#include "tabs.h"

const int DIR_SIZE_COLUMN_WIDTH = mul_by_system_scaling_factor(70);
const int FILES_COUNT_COLUMN_WIDTH = mul_by_system_scaling_factor(50);
const int LINE_HEIGHT = FONT_HEIGHT + 2;

std::unordered_set<std::wstring> always_excluded_directories = {
    L"$Recycle.Bin",
    L"Program Files",
    L"Program Files (x86)",
    L"Users",
    L"Windows",
};
// By default ‘<UserProfile>\AppData\Local’ and ‘<UserProfile>\AppData\LocalLow’ are excluded
// C/<UserProfile> <- %USERPROFILE% or [UserProfiles]/<UserName>
//   ^           ^
//   └───────────┴─────────────────────────────────── — because `<` and `>` are not allowed in the file name

class DirEntry
{
public:
    DirEntry *parent = nullptr;
    std::map<std::wstring, DirEntry> subdirs;
    SpinLock subdirs_lock;
    uint32_t num_of_files = 0;
    uint64_t size = 0;
    bool scan_started = false;
};

DirEntry root_dir_entry;

void enum_files_recursively(const std::wstring &dir_name, DirEntry &de)
{
    de.scan_started = true;

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile((dir_name / L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::map<std::wstring, DirEntry> subdirs;

    do
    {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_REPARSE_POINT)) // skip hidden files and directories and symbolic links
            continue;

        std::wstring file_name(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (file_name == L"." || file_name == L"..")
                continue;
            if (!de.parent && always_excluded_directories.find(file_name) != always_excluded_directories.end())
                continue;
            subdirs[file_name].parent = &de;
        }
        else {
            de.size += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            de.num_of_files++;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);

    de.subdirs_lock.acquire();
    de.subdirs = std::move(subdirs);
    de.subdirs_lock.release();

    for (DirEntry *pde = de.parent; pde != nullptr; pde = pde->parent) {
        pde->size += de.size;
        pde->num_of_files += de.num_of_files;
    }

    for (auto &&sd : de.subdirs)
        enum_files_recursively(dir_name / sd.first, sd.second);
}

DWORD WINAPI initial_scan(LPVOID)
{
    enum_files_recursively(L"C:", root_dir_entry);
    return 0;
}

void TabBackup::treeview_paint(HDC hdc, int width, int height)
{
    std::vector<std::pair<const std::wstring*, DirEntry*>> dirs;
    root_dir_entry.subdirs_lock.acquire();
    dirs.reserve(root_dir_entry.subdirs.size());
    for (auto &&d : root_dir_entry.subdirs)
        dirs.push_back(std::make_pair(&d.first, &d.second));
    root_dir_entry.subdirs_lock.release();

    // Draw hover rect
    POINT cur_pos;
    GetCursorPos(&cur_pos);
    RECT wnd_rect;
    GetWindowRect(treeview_wnd, &wnd_rect);
    if (cur_pos.x >= wnd_rect.left
     && cur_pos.y >= wnd_rect.top
     && cur_pos.x < wnd_rect.right
     && cur_pos.y < wnd_rect.bottom) {
        int item_under_mouse = (cur_pos.y - wnd_rect.top - TREEVIEW_PADDING) / LINE_HEIGHT;
        if (item_under_mouse < (int)dirs.size()) {
            SelectPenAndBrush spb(hdc, RGB(112, 192, 231), RGB(229, 243, 251)); // colors are taken from Windows Explorer
            Rectangle(hdc, TREEVIEW_PADDING, TREEVIEW_PADDING + item_under_mouse * LINE_HEIGHT, width - TREEVIEW_PADDING, TREEVIEW_PADDING + (item_under_mouse+1) * LINE_HEIGHT);
        }
    }

    // Draw tree view items
    SelectFont(hdc, treeview_font);
    SetBkMode(hdc, TRANSPARENT);

    RECT r;
    r.top = TREEVIEW_PADDING + 1;
    for (auto &&d : dirs) {
        r.bottom = r.top + FONT_HEIGHT;

        r.right = width - TREEVIEW_PADDING - 2;
        r.left = r.right - DIR_SIZE_COLUMN_WIDTH;
        if (d.second->scan_started) {
            const int MB = 1024 * 1024;
            char s[32];
            //sprintf_s(s, d.second->size >= 100*MB ? "%.0f" : d.second->size >= 10*MB ? "%.1f" : d.second->size >= MB ? "%.2f" : "%.3f", d.second->size / double(MB));
            sprintf_s(s, "%.1f", d.second->size / double(MB));
            DrawTextA(hdc, s, -1, &r, DT_RIGHT);

            r.right = r.left;
            r.left -= FILES_COUNT_COLUMN_WIDTH;
            _itoa_s(d.second->num_of_files, s, 10);
            DrawTextA(hdc, s, -1, &r, DT_RIGHT);
        }
        else
            DrawText(hdc, L"?", -1, &r, DT_RIGHT);

        r.right = r.left;
        r.left = TREEVIEW_PADDING + 2;
        DrawText(hdc, d.first->c_str(), -1, &r, DT_END_ELLIPSIS);

        r.top += LINE_HEIGHT;
    }
}
