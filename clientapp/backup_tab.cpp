#include "precompiled.h"
#include "tabs.h"

const int DIR_SIZE_COLUMN_WIDTH = mul_by_system_scaling_factor(70);
const int FILES_COUNT_COLUMN_WIDTH = mul_by_system_scaling_factor(50);
const int TREEVIEW_LEVEL_OFFSET = mul_by_system_scaling_factor(16);

volatile bool TabBackup::stop_scan = false;

std::unordered_set<std::wstring> always_excluded_directories = {
    L"$Recycle.Bin",
    L"Program Files",
    L"Program Files (x86)",
    L"Users",
    L"Windows",
};
// By default ‘<UserProfile>\AppData\Local’ and ‘<UserProfile>\AppData\LocalLow’ are excluded (also .git directories which size is more than 100MB are excluded)
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
    bool expanded = false;
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
        if (TabBackup::stop_scan) {
            FindClose(h);
            return;
        }

        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_REPARSE_POINT)) // skip hidden files and directories and symbolic links
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".git") == 0)
                ASSERT((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
            else
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

    for (auto &&sd : de.subdirs) {
        if (TabBackup::stop_scan)
            return;
        enum_files_recursively(dir_name / sd.first, sd.second);
    }
}

DWORD WINAPI initial_scan(LPVOID)
{
    enum_files_recursively(L"C:", root_dir_entry);
    return 0;
}

struct DirItem
{
    const std::wstring *name;
    DirEntry *d;
    int level;
};

void fill_dirs(std::vector<DirItem> &all_dirs, DirEntry &d, int level = 0)
{
    std::vector<DirItem> dirs;
    d.subdirs_lock.acquire();
    dirs.reserve(d.subdirs.size());
    for (auto &&dir : d.subdirs) {
        DirItem di = {&dir.first, &dir.second, level};
        dirs.push_back(di);
    }
    d.subdirs_lock.release();

    for (auto &&di : dirs) {
        all_dirs.push_back(di);
        if (di.d->expanded)
            fill_dirs(all_dirs, *di.d, level + 1);
    }
}

POINT pressed_cur_pos;
bool operator==(const POINT &p1, const POINT &p2) {return memcmp(&p1, &p2, sizeof(POINT)) == 0;}
DirItem treeview_hover_dir_item = {0};

void TabBackup::treeview_paint(HDC hdc, int width, int height)
{
    std::vector<DirItem> dirs;
    fill_dirs(dirs, root_dir_entry);

    int new_smax = dirs.size()*LINE_HEIGHT + TREEVIEW_PADDING*2 - 2; // without `- 2` there are artifacts at the bottom of tree view after scrolling to end and scrollbar up button pressed
    int smin, smax;
    ScrollBar_GetRange(scrollbar_wnd, &smin, &smax);
    if (smax != new_smax) {
        ScrollBar_SetRange(scrollbar_wnd, 0, new_smax, FALSE);
        PostMessage(main_wnd, WM_SIZE, 0, 0);
    }

    int scrollpos = ScrollBar_GetPos(scrollbar_wnd);

    // Draw hover rect
    treeview_hover_dir_item.d = nullptr;
    POINT cur_pos;
    GetCursorPos(&cur_pos);
    RECT wnd_rect;
    GetWindowRect(treeview_wnd, &wnd_rect);
    if (cur_pos.x >= wnd_rect.left
     && cur_pos.y >= wnd_rect.top
     && cur_pos.x < wnd_rect.right
     && cur_pos.y < wnd_rect.bottom
     && (GetAsyncKeyState(VK_LBUTTON) >= 0 || cur_pos == pressed_cur_pos)) { // do not show hover rect when left mouse button is pressed (during scrolling or pressing some button)
        int item_under_mouse = (cur_pos.y - wnd_rect.top - TREEVIEW_PADDING + scrollpos) / LINE_HEIGHT;
        if (item_under_mouse < (int)dirs.size()) {
            treeview_hover_dir_item = dirs[item_under_mouse];
            SelectPenAndBrush spb(hdc, RGB(112, 192, 231), RGB(229, 243, 251)); // colors are taken from Windows Explorer
            Rectangle(hdc, TREEVIEW_PADDING + treeview_hover_dir_item.level * TREEVIEW_LEVEL_OFFSET + ICON_SIZE,
                           TREEVIEW_PADDING - scrollpos +  item_under_mouse    * LINE_HEIGHT, width - TREEVIEW_PADDING,
                           TREEVIEW_PADDING - scrollpos + (item_under_mouse+1) * LINE_HEIGHT);
        }
    }

    // Draw tree view items
    SelectFont(hdc, treeview_font);
    SetBkMode(hdc, TRANSPARENT);

    RECT r;
    r.top = TREEVIEW_PADDING + LINE_PADDING_TOP - scrollpos;
    for (auto &&d : dirs) {
        if (r.top >= height)
            break;

        r.bottom = r.top + FONT_HEIGHT;

        if (r.bottom >= TREEVIEW_PADDING) { // this check is not only for better performance, but is also to avoid artifacts at the top of tree view after scrollbar down button pressed
            r.right = width - TREEVIEW_PADDING - LINE_PADDING_RIGHT;
            r.left = r.right - DIR_SIZE_COLUMN_WIDTH;
            if (d.d->scan_started) {
                const int MB = 1024 * 1024;
                char s[32];
                //sprintf_s(s, d.second->size >= 100*MB ? "%.0f" : d.second->size >= 10*MB ? "%.1f" : d.second->size >= MB ? "%.2f" : "%.3f", d.second->size / double(MB));
                sprintf_s(s, "%.1f", d.d->size / double(MB));
                DrawTextA(hdc, s, -1, &r, DT_RIGHT);

                r.right = r.left;
                r.left -= FILES_COUNT_COLUMN_WIDTH;
                _itoa_s(d.d->num_of_files, s, 10);
                DrawTextA(hdc, s, -1, &r, DT_RIGHT);
            }
            else
                DrawText(hdc, L"?", -1, &r, DT_RIGHT);

            r.right = r.left;
            r.left = TREEVIEW_PADDING + d.level * TREEVIEW_LEVEL_OFFSET;
            DrawIconEx(hdc, r.left, r.top, d.d->expanded ? icon_dir_exp : icon_dir_col, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);

            r.left += ICON_SIZE + LINE_PADDING_LEFT;
            DrawText(hdc, d.name->c_str(), -1, &r, DT_END_ELLIPSIS);
        }

        r.top += LINE_HEIGHT;
    }
}

void TabBackup::treeview_lbdown()
{
    GetCursorPos(&pressed_cur_pos);
    if (treeview_hover_dir_item.d == nullptr)
        return;

    treeview_hover_dir_item.d->expanded = !treeview_hover_dir_item.d->expanded;
    InvalidateRect(treeview_wnd, NULL, FALSE);
}
