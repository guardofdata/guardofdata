#include "precompiled.h"
#include "tabs.h"

const int DIR_SIZE_COLUMN_WIDTH = mul_by_system_scaling_factor(70);
const int FILES_COUNT_COLUMN_WIDTH = mul_by_system_scaling_factor(50);
const int TREEVIEW_LEVEL_OFFSET = mul_by_system_scaling_factor(16);
const int DIR_MODE_LEVELS_AUTO = 2;

TabBackup::SortBy TabBackup::sort_by = SortBy::NAME;
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

enum class DirMode
{
    INHERIT_FROM_PARENT,
    EXCLUDED,
    NORMAL,
    FROZEN,
    APPEND_ONLY,
};
const float DIR_PRIORITY_ULTRA_HIGH =  2;
const float DIR_PRIORITY_HIGH       =  1;
const float DIR_PRIORITY_NORMAL     =  0;
const float DIR_PRIORITY_LOW        = -1;
const float DIR_PRIORITY_ULTRA_LOW  = -2;
HICON mode_icons[4], mode_mixed_icon, priority_icons[4];

class DirEntry
{
public:
    DirEntry *parent = nullptr;
    struct Less
    {
        wchar_t fast_get_lowercase_en(wchar_t c) const
        {
            if (unsigned(int(c) - int(L'A')) <= unsigned(L'Z' - L'A'))
                return c + (L'a' - L'A');
            return c;
        }

        bool operator()(const std::wstring &left, const std::wstring &right) const
        {
            for (const wchar_t *l = left.c_str(), *r = right.c_str(); ; l++, r++) {
                wchar_t lower_l = fast_get_lowercase_en(*l),
                        lower_r = fast_get_lowercase_en(*r);
                if (lower_l != lower_r)
                    return lower_l < lower_r;
                if (lower_l == 0)
                    return false;
            }
        }
    };
    using SubDirs = std::map<std::wstring, DirEntry, Less>;
    SubDirs subdirs;
    SpinLock subdirs_lock;
    uint32_t num_of_files = 0;
    uint32_t num_of_files_excluded = 0;
    uint64_t size = 0;
    uint64_t size_excluded = 0;
    FILETIME max_last_write_time = FILETIME{}; // zero initialization
    DirMode mode = DirMode::INHERIT_FROM_PARENT;
    float priority = DIR_PRIORITY_NORMAL;
    bool mode_mixed = false;
    bool scan_started = false;
    bool expanded = false;
    //bool deleted = false;
};

DirEntry root_dir_entry;
FILETIME cur_ft;

void enum_files_recursively(const std::wstring &dir_name, DirEntry &de, int level = 0)
{
    de.scan_started = true;

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile((dir_name / L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    DirEntry::SubDirs subdirs;

    do
    {
        if (TabBackup::stop_scan) {
            FindClose(h);
            return;
        }

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) && !(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L"desktop.ini") == 0))
            continue;

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
            if ((uint64_t&)fd.ftLastWriteTime > (uint64_t&)de.max_last_write_time // CompareFileTime() is much slower
                    && (int64_t&)cur_ft - (int64_t&)fd.ftLastWriteTime >= 0) // ignore time in future
                de.max_last_write_time = fd.ftLastWriteTime;
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
        enum_files_recursively(dir_name / sd.first, sd.second, level + 1);
        if ((uint64_t&)sd.second.max_last_write_time > (uint64_t&)de.max_last_write_time)
            de.max_last_write_time = sd.second.max_last_write_time;
    }

    std::function<void(DirEntry&)> set_inherit_from_parent_and_excluded = [&set_inherit_from_parent_and_excluded](DirEntry &de) {
        de.size_excluded = de.size;
        de.num_of_files_excluded = de.num_of_files;
        for (auto &&sd : de.subdirs) {
            sd.second.mode = DirMode::INHERIT_FROM_PARENT;
            set_inherit_from_parent_and_excluded(sd.second);
        }
    };
    if (ends_with(dir_name, L"/.git") && de.size > 100*1024*1024) {
        de.mode = DirMode::EXCLUDED;
        set_inherit_from_parent_and_excluded(de);
        for (DirEntry *pde = de.parent; pde != nullptr; pde = pde->parent) {
            pde->size_excluded += de.size;
            pde->num_of_files_excluded += de.num_of_files;
        }
        return;
    }

    if (level > DIR_MODE_LEVELS_AUTO) {
        de.mode = DirMode::INHERIT_FROM_PARENT;
        //return; // no return to check if mode is mixed (e.g. if there is EXCLUDED .git subdirectory)
    }
    else {
        size_t last_slash_pos = dir_name.rfind(L'/');
        if (last_slash_pos != dir_name.npos) {
            std::wstring base_name(dir_name.c_str() + last_slash_pos + 1);
            fast_make_lowercase_en(&base_name[0]);
            if (base_name.find(L"photo") != base_name.npos) {
                de.mode = DirMode::APPEND_ONLY;
                std::function<void(DirEntry&)> set_inherit_from_parent = [&set_inherit_from_parent](DirEntry &de) {
                    for (auto &&sd : de.subdirs) {
                        sd.second.mode = DirMode::INHERIT_FROM_PARENT;
                        set_inherit_from_parent(sd.second);
                    }
                };
                set_inherit_from_parent(de);
                return;
            }
        }

        if ((uint64_t&)de.max_last_write_time == 0) {
            de.mode = DirMode::INHERIT_FROM_PARENT;
            return;
        }

        std::function<void(DirEntry&)> set_priority_to_normal = [&set_priority_to_normal](DirEntry &de) {
            for (auto &&sd : de.subdirs) {
                sd.second.priority = DIR_PRIORITY_NORMAL;
                set_priority_to_normal(sd.second);
            }
        };
        int64_t days_since_last_write = ((int64_t&)cur_ft - (int64_t&)de.max_last_write_time)/(10000000LL*3600*24);
        if (days_since_last_write > 365/2) {
            de.mode = DirMode::FROZEN;
            if (/*level == 1 && */de.size > 10*1024*1024) {
                de.priority = /*days_since_last_write < 365 ? */DIR_PRIORITY_LOW/* : DIR_PRIORITY_ULTRA_LOW*/; // there is very little data changed from six months to a year ago, and besides, it makes sense to reserve an ultra low priority for manual selection by the user
                set_priority_to_normal(de);
            }
        }
        else {
            de.mode = DirMode::NORMAL;
            if (days_since_last_write <= 7 && de.size <= 1024*1024*1024) {
                de.priority = DIR_PRIORITY_HIGH;
                set_priority_to_normal(de);
            }
        }
    }

    // Check if mode is mixed
    for (auto &&sd : de.subdirs)
        if ((sd.second.mode != de.mode && sd.second.mode != DirMode::INHERIT_FROM_PARENT) || sd.second.mode_mixed) {
            de.mode_mixed = true;
            break;
        }
}

DWORD WINAPI initial_scan(LPVOID)
{
    GetSystemTimeAsFileTime(&cur_ft);
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

    if (TabBackup::sort_by == TabBackup::SortBy::SIZE)
        std::sort(dirs.begin(), dirs.end(), [](const DirItem &a, const DirItem &b) {
            if (a.d->num_of_files_excluded == a.d->num_of_files
             && b.d->num_of_files_excluded == b.d->num_of_files)
                return a.d->size > b.d->size;
            return a.d->size - a.d->size_excluded > b.d->size - b.d->size_excluded;
        });

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
    int item_under_mouse;
    POINT cur_pos;
    GetCursorPos(&cur_pos);
    RECT wnd_rect;
    GetWindowRect(treeview_wnd, &wnd_rect);
    if (cur_pos.x >= wnd_rect.left
     && cur_pos.y >= wnd_rect.top
     && cur_pos.x < wnd_rect.right
     && cur_pos.y < wnd_rect.bottom
     && (GetAsyncKeyState(VK_LBUTTON) >= 0 || cur_pos == pressed_cur_pos)) { // do not show hover rect when left mouse button is pressed (during scrolling or pressing some button)
        item_under_mouse = (cur_pos.y - wnd_rect.top - TREEVIEW_PADDING + scrollpos) / LINE_HEIGHT;
        if (item_under_mouse < (int)dirs.size()) {
            treeview_hover_dir_item = dirs[item_under_mouse];
            SelectPenAndBrush spb(hdc, RGB(112, 192, 231), RGB(229, 243, 251)); // colors are taken from Windows Explorer
            Rectangle(hdc, TREEVIEW_PADDING + treeview_hover_dir_item.level * TREEVIEW_LEVEL_OFFSET + ICON_SIZE - 1,
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
                sprintf_s(s, "%.1f", (d.d->num_of_files_excluded == d.d->num_of_files ? d.d->size_excluded : d.d->size - d.d->size_excluded) / double(MB));
                //sprintf_s(s, "%.1f", ((int64_t&)cur_ft - (int64_t&)d.d->max_last_write_time)/(10000000.0*3600*24));
                COLORREF prev_text_color;
                if (d.d->num_of_files_excluded > 0)
                    prev_text_color = SetTextColor(hdc, d.d->num_of_files_excluded == d.d->num_of_files ? RGB(192, 0, 0) : RGB(192, 192, 0));
                DrawTextA(hdc, s, -1, &r, DT_RIGHT);

                r.right = r.left;
                r.left -= FILES_COUNT_COLUMN_WIDTH;
                _itoa_s(d.d->num_of_files_excluded == d.d->num_of_files ? d.d->num_of_files_excluded : d.d->num_of_files - d.d->num_of_files_excluded, s, 10);
                DrawTextA(hdc, s, -1, &r, DT_RIGHT);
                if (d.d->num_of_files_excluded > 0)
                    SetTextColor(hdc, prev_text_color);
            }
            else
                DrawText(hdc, L"?", -1, &r, DT_RIGHT);

            r.right = r.left;
            r.left = TREEVIEW_PADDING + d.level * TREEVIEW_LEVEL_OFFSET;
            if (!d.d->subdirs.empty())
                DrawIconEx(hdc, r.left, r.top, d.d->expanded ? icon_dir_exp : icon_dir_col, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);

            r.left += ICON_SIZE;
            if (d.d->mode_mixed) {
                //DrawIconEx(hdc, r.left, r.top, mode_mixed_icon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
                static HPEN pen = CreatePen(PS_SOLID, 1, RGB(249, 236, 0));
                SelectPen(hdc, pen);
                SelectBrush(hdc, GetStockBrush(HOLLOW_BRUSH));
                Rectangle(hdc, r.left, r.top, r.left + ICON_SIZE, r.top + ICON_SIZE);
            }
            DirMode mode = d.d->mode;
            if (mode == DirMode::INHERIT_FROM_PARENT)
                for (DirEntry *pd = d.d->parent; pd; pd = pd->parent)
                    if (pd->mode != DirMode::INHERIT_FROM_PARENT) {
                        mode = pd->mode;
                        break;
                    }
            if (mode != DirMode::INHERIT_FROM_PARENT)
                DrawIconEx(hdc, r.left, r.top, mode_icons[(int)mode-1], ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);

            if (d.d->priority != DIR_PRIORITY_NORMAL) {
                r.left += ICON_SIZE;
                DrawIconEx(hdc, r.left, r.top, priority_icons[(d.d->priority > 0 ? 2 : 1) - (int)d.d->priority], ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
            }

            r.left += ICON_SIZE + LINE_PADDING_LEFT;
            DrawText(hdc, d.name->c_str(), -1, &r, DT_END_ELLIPSIS);
        }

        r.top += LINE_HEIGHT;
    }

    if (treeview_hover_dir_item.d != nullptr
     && treeview_hover_dir_item.d->num_of_files_excluded > 0
     && treeview_hover_dir_item.d->num_of_files_excluded != treeview_hover_dir_item.d->num_of_files) {
        int i = item_under_mouse + 1, m = 1, p = 0;
        SelectPenAndBrush spb(hdc, RGB(112, 192, 231), RGB(229, 243, 251)); // colors are taken from Windows Explorer
        if (TREEVIEW_PADDING - scrollpos + (i+1) * LINE_HEIGHT >= wnd_rect.bottom - wnd_rect.top) { // with `>` bottom outline can disappear (when trying to see this[‘difference between `>` and `>=`’] be aware that single pixel movement of scrollbar thumb can lead to change of `scrollpos` by 2)
            i = item_under_mouse - 1;
            m = 0;
            p = 1;
        }
        RECT r = {width - TREEVIEW_PADDING - DIR_SIZE_COLUMN_WIDTH - FILES_COUNT_COLUMN_WIDTH,
                          TREEVIEW_PADDING - scrollpos +  i    * LINE_HEIGHT, width - TREEVIEW_PADDING,
                          TREEVIEW_PADDING - scrollpos + (i+1) * LINE_HEIGHT};
        Rectangle(hdc, r.left, r.top - m, r.right, r.bottom + p);
        r.top += LINE_PADDING_TOP;
        r.bottom = r.top + FONT_HEIGHT;
        r.right = width - TREEVIEW_PADDING - LINE_PADDING_RIGHT;
        r.left = r.right - DIR_SIZE_COLUMN_WIDTH;
        char s[32];
        sprintf_s(s, "%.1f", treeview_hover_dir_item.d->size_excluded / double(1024*1024));
        SetTextColor(hdc, RGB(192, 0, 0));
        DrawTextA(hdc, s, -1, &r, DT_RIGHT);

        r.right = r.left;
        r.left -= FILES_COUNT_COLUMN_WIDTH;
        _itoa_s(treeview_hover_dir_item.d->num_of_files_excluded, s, 10);
        DrawTextA(hdc, s, -1, &r, DT_RIGHT);
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

void TabBackup::treeview_rbdown()
{
    HMENU menu = LoadMenu(h_instance, MAKEINTRESOURCE(IDR_BACKUP_TAB_CONTEXT_MENU));
    CheckMenuRadioItem(menu, ID_SORTBY_NAME, ID_SORTBY_NAME + (int)SortBy::COUNT - 1, ID_SORTBY_NAME + (int)sort_by, MF_BYCOMMAND);
    HMENU sub_menu = GetSubMenu(menu, 0); // this is necessary (see [http://rsdn.org/article/qna/ui/mnuerr1.xml <- http://rsdn.org/forum/winapi/140595.flat <- google:‘TrackPopupMenu "view as popup"’])
    POINT curpos;
    GetCursorPos(&curpos);
    auto r = TrackPopupMenu(sub_menu, TPM_LEFTALIGN|TPM_BOTTOMALIGN|TPM_RIGHTBUTTON|TPM_NONOTIFY|TPM_RETURNCMD, curpos.x, curpos.y, 0, treeview_wnd, NULL);
    if (r != 0)
        sort_by = SortBy(r - ID_SORTBY_NAME);
    DestroyMenu(menu);
}
