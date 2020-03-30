#include "precompiled.h"
#include "tabs.h"

const int DIR_SIZE_COLUMN_WIDTH = mul_by_system_scaling_factor(70);
const int FILES_COUNT_COLUMN_WIDTH = mul_by_system_scaling_factor(52);
const int TREEVIEW_LEVEL_OFFSET = mul_by_system_scaling_factor(16);
const int DIR_MODE_LEVELS_AUTO = 3;

TabBackup::SortBy TabBackup::sort_by = SortBy::NAME;
volatile bool TabBackup::stop_scan = false, TabBackup::cancel_scan = false;
bool popup_menu_is_open = false;

std::unordered_set<std::wstring> always_excluded_directories = {
    L"$Recycle.Bin",
    L"Program Files",
    L"Program Files (x86)",
    L"Users",
    L"Windows",
    // For Windows XP:
    L"WINDOWS",
    L"Documents and Settings",
};
// By default ‘<UserProfile>\AppData\Local’ and ‘<UserProfile>\AppData\LocalLow’ are excluded (also .git directories which size is more than 100MB are excluded)
// C/<UserProfile> <- %USERPROFILE% or [UserProfiles]/<UserName>
//   ^           ^
//   └───────────┴─────────────────────────────────── — because `<` and `>` are not allowed in the file name

enum class DirMode
{
    EXCLUDED,
    NORMAL,
    FROZEN,
    APPEND_ONLY,
    INHERIT_FROM_PARENT,
    AUTO,
    COUNT
};
const float DIR_PRIORITY_ULTRA_HIGH =  2;
const float DIR_PRIORITY_HIGH       =  1;
const float DIR_PRIORITY_NORMAL     =  0;
const float DIR_PRIORITY_LOW        = -1;
const float DIR_PRIORITY_ULTRA_LOW  = -2;
const float DIR_PRIORITY_AUTO       = FLT_MIN;
HICON mode_icons[4], mode_mixed_icon, mode_manual_icon, priority_icons[4];
HBITMAP mode_bitmaps[4], mode_bitmaps_selected[4];
HBITMAP priority_bitmaps[5], priority_bitmaps_selected[5];

class DirEntry
{
public:
    DirEntry *parent = nullptr;
    const std::wstring *dir_name = nullptr;
    std::wstring full_dir_name() const
    {
        std::wstring full_dir_name = *dir_name;
        for (DirEntry *p = parent; p; p = p->parent)
            full_dir_name = *p->dir_name / full_dir_name;
        return full_dir_name;
    }
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
    int32_t dir_num_of_files = 0; // number of files just in this directory
    int32_t num_of_files = 0; // total number of files including subdirectories
    int32_t num_of_files_excluded = 0;
    int64_t dir_files_size = 0; // size of files just in this directory
    int64_t size = 0; // total size of files including subdirectories
    int64_t size_excluded = 0;
    FILETIME max_last_write_time = FILETIME{}; // zero initialization
    DirMode mode_auto = DirMode::INHERIT_FROM_PARENT;
    DirMode mode_manual = DirMode::AUTO;
    DirMode mode() const {return mode_manual != DirMode::AUTO ? mode_manual : mode_auto;}
    DirMode mode_no_ifp() const
    {
        if (mode() != DirMode::INHERIT_FROM_PARENT)
            return mode();
        for (DirEntry *pd = parent; pd; pd = pd->parent)
            if (pd->mode() != DirMode::INHERIT_FROM_PARENT)
                return pd->mode();
        return DirMode::INHERIT_FROM_PARENT;
    }
    float priority_auto = DIR_PRIORITY_NORMAL;
    float priority_manual = DIR_PRIORITY_AUTO;
    float priority() const {return priority_manual == DIR_PRIORITY_AUTO ? priority_auto : priority_manual;}
    bool mode_mixed = false;
    bool scan_started = false;
    bool expanded = false;
    //bool deleted = false;
    bool not_traversed = false; // directory entry was not scanned

    void update_mode_mixed()
    {
        for (DirEntry *pd = parent; pd; pd = pd->parent) {
            pd->mode_mixed = false;
            DirMode pd_mode_no_ifp = pd->mode_no_ifp();
            for (auto &&sd : pd->subdirs)
                if ((sd.second.mode() != pd_mode_no_ifp && sd.second.mode() != DirMode::INHERIT_FROM_PARENT) || sd.second.mode_mixed) {
                    pd->mode_mixed = true;
                    break;
                }
        }
    }

    void exclude_auto(bool set_priority_to_normal_and_update_mode_mixed = false)
    {
        std::function<void(DirEntry&)> set_inherit_from_parent_and_excluded = [&set_inherit_from_parent_and_excluded, set_priority_to_normal_and_update_mode_mixed](DirEntry &de) {
            if (set_priority_to_normal_and_update_mode_mixed)
                de.priority_auto = DIR_PRIORITY_NORMAL;
            de.size_excluded = de.size;
            de.num_of_files_excluded = de.num_of_files;
            for (auto &&sd : de.subdirs) {
                sd.second.mode_auto = DirMode::INHERIT_FROM_PARENT;
                set_inherit_from_parent_and_excluded(sd.second);
            }
        };

        mode_auto = DirMode::EXCLUDED;
        set_inherit_from_parent_and_excluded(*this);
        for (DirEntry *pde = parent; pde != nullptr; pde = pde->parent) {
            pde->size_excluded += size;
            pde->num_of_files_excluded += num_of_files;
        }

        if (set_priority_to_normal_and_update_mode_mixed)
            update_mode_mixed();
    }

    void set_mode_manual(DirMode new_mode_manual)
    {
        DirMode prev_mode_no_ifp = mode_no_ifp();
        mode_manual = new_mode_manual;

        // Update `mode_mixed`
        update_mode_mixed();

        // Update `num_of_files_excluded` and `size_excluded` if necessary
        if ((prev_mode_no_ifp == DirMode::EXCLUDED) != (mode_no_ifp() == DirMode::EXCLUDED)) {
            int64_t prev_size_excluded         = size_excluded;
            int32_t prev_num_of_files_excluded = num_of_files_excluded;
            std::function<void(DirEntry&)> recalc_excluded = [&recalc_excluded](DirEntry &de) {
                if (de.mode_no_ifp() == DirMode::EXCLUDED) {
                    de.size_excluded = de.dir_files_size;
                    de.num_of_files_excluded = de.dir_num_of_files;
                }
                else {
                    de.size_excluded = 0;
                    de.num_of_files_excluded = 0;
                }
                for (auto &&sd : de.subdirs) {
                    recalc_excluded(sd.second);
                    de.size_excluded += sd.second.size_excluded;
                    de.num_of_files_excluded += sd.second.num_of_files_excluded;
                }
            };
            recalc_excluded(*this);
            int64_t delta_size_excluded         = size_excluded         - prev_size_excluded;
            int32_t delta_num_of_files_excluded = num_of_files_excluded - prev_num_of_files_excluded;
            for (DirEntry *pde = parent; pde != nullptr; pde = pde->parent) {
                pde->size_excluded += delta_size_excluded;
                pde->num_of_files_excluded += delta_num_of_files_excluded;
            }
        }
    }
};

class RootDirEntry : public DirEntry
{
public:
    std::wstring path, name;

    RootDirEntry(const std::wstring &path) : path(path)
    {
        if (path.back() == L':') {
            wchar_t label[MAX_PATH+1] = L"\0";
            GetVolumeInformation((path + L'\\').c_str(), label, _countof(label), NULL, NULL, NULL, NULL, 0);
            if (label[0] != L'\0')
                name = path + L" [" + label + L']';
            else
                name = path;
        }
        else
            name = path;
        dir_name = &this->path;
    }
    RootDirEntry(const std::wstring &path, const std::wstring &name) : path(path), name(name) {dir_name = &this->path;}
};
std::vector<std::unique_ptr<RootDirEntry>> root_dir_entries;
class InitRootDirEntries
{
public:
    InitRootDirEntries()
    {
        root_dir_entries.push_back(std::make_unique<RootDirEntry>(L"C:"));
        root_dir_entries.back()->expanded = true;

        wchar_t user_profile_dir[MAX_PATH];
        size_t rsz;
        _wgetenv_s(&rsz, user_profile_dir, L"USERPROFILE");
        root_dir_entries.push_back(std::make_unique<RootDirEntry>(user_profile_dir, L"<UserProfile>"));
    }
} init_root_dir_entries;

FILETIME cur_ft;

void enum_files_recursively(const std::wstring &dir_name, DirEntry &de, int level)
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
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (wcscmp(fd.cFileName, L".git") == 0 || wcscmp(fd.cFileName, L"AppData") == 0))
                ASSERT((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
            else
                continue;

        std::wstring file_name(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (file_name == L"." || file_name == L"..")
                continue;
            if (!de.parent && always_excluded_directories.find(file_name) != always_excluded_directories.end())
                continue;

            auto it = subdirs.emplace(file_name, DirEntry());
            it.first->second.parent = &de;
            it.first->second.dir_name = &it.first->first;
        }
        else {
            de.dir_files_size += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            de.dir_num_of_files++;
            if ((uint64_t&)fd.ftLastWriteTime > (uint64_t&)de.max_last_write_time // CompareFileTime() is much slower
                    && (int64_t&)cur_ft - (int64_t&)fd.ftLastWriteTime >= 0) // ignore time in future
                de.max_last_write_time = fd.ftLastWriteTime;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);

    de.size = de.dir_files_size;
    de.num_of_files = de.dir_num_of_files;

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

    if (ends_with(dir_name, L"/.git") && de.size > 100*1024*1024) {
        de.exclude_auto();
        return;
    }

    if (level > DIR_MODE_LEVELS_AUTO) {
        de.mode_auto = DirMode::INHERIT_FROM_PARENT;
        //return; // no return to check if mode is mixed (e.g. if there is EXCLUDED .git subdirectory)
    }
    else {
        size_t last_slash_pos = dir_name.rfind(L'/');
        if (last_slash_pos != dir_name.npos) {
            std::wstring base_name(dir_name.c_str() + last_slash_pos + 1);
            fast_make_lowercase_en(&base_name[0]);
            if (base_name.find(L"photo") != base_name.npos || base_name.find(L"backup") != base_name.npos) {
                de.mode_auto = DirMode::APPEND_ONLY;
                de.priority_auto = DIR_PRIORITY_LOW;
                std::function<void(DirEntry&)> set_inherit_from_parent = [&set_inherit_from_parent](DirEntry &de) {
                    for (auto &&sd : de.subdirs) {
                        sd.second.mode_auto = DirMode::INHERIT_FROM_PARENT;
                        sd.second.priority_auto = DIR_PRIORITY_NORMAL;
                        set_inherit_from_parent(sd.second);
                    }
                };
                set_inherit_from_parent(de);
                return;
            }
        }

        if ((uint64_t&)de.max_last_write_time == 0) {
            de.mode_auto = DirMode::INHERIT_FROM_PARENT;
            return;
        }

        std::function<void(DirEntry&)> set_priority_to_normal = [&set_priority_to_normal](DirEntry &de) {
            for (auto &&sd : de.subdirs) {
                sd.second.priority_auto = DIR_PRIORITY_NORMAL;
                set_priority_to_normal(sd.second);
            }
        };
        int64_t days_since_last_write = ((int64_t&)cur_ft - (int64_t&)de.max_last_write_time)/(10000000LL*3600*24);
        if (days_since_last_write > 365/2) {
            de.mode_auto = DirMode::FROZEN;
            if (/*level == 1 && */de.size > 10*1024*1024) {
                de.priority_auto = /*days_since_last_write < 365 ? */DIR_PRIORITY_LOW/* : DIR_PRIORITY_ULTRA_LOW*/; // there is very little data changed from six months to a year ago, and besides, it makes sense to reserve an ultra low priority for manual selection by the user
                set_priority_to_normal(de);
            }
        }
        else {
            de.mode_auto = DirMode::NORMAL;
            if (days_since_last_write <= 7 && de.size <= 1024*1024*1024) {
                de.priority_auto = DIR_PRIORITY_HIGH;
                set_priority_to_normal(de);
            }
        }
    }

    // Check if mode is mixed
    for (auto &&sd : de.subdirs)
        if ((sd.second.mode_auto != de.mode_auto && sd.second.mode_auto != DirMode::INHERIT_FROM_PARENT) || sd.second.mode_mixed) {
            de.mode_mixed = true;
            break;
        }
}

HANDLE initial_scan_thread, scan_thread = NULL;

void cancel_scan();

DWORD WINAPI initial_scan(LPVOID)
{
    GetSystemTimeAsFileTime(&cur_ft);
    for (auto &root_dir_entry : root_dir_entries) {
        enum_files_recursively(root_dir_entry->path, *root_dir_entry, 1);
        if (TabBackup::stop_scan) {
            if (TabBackup::cancel_scan)
                cancel_scan();
            return 1;
        }
    }

    // Exclude ‘<UserProfile>\AppData\Local’ and ‘<UserProfile>\AppData\LocalLow’
    RootDirEntry &upde = *root_dir_entries[1];
    ASSERT(upde.name == L"<UserProfile>");
    auto it = upde.subdirs.find(L"AppData");
    if (it != upde.subdirs.end()) { // for Windows XP
        auto  lit = it->second.subdirs.find(L"Local");    if ( lit != it->second.subdirs.end())  lit->second.exclude_auto(true);
        auto llit = it->second.subdirs.find(L"LocalLow"); if (llit != it->second.subdirs.end()) llit->second.exclude_auto(true);
    }

    backup_state = BackupState::SCAN_COMPLETED;
    SendMessage(main_wnd, WM_COMMAND, IDB_TAB_BACKUP, 0); // needed to update backup tab if it is already active

    MessageBox(main_wnd, L"Scan completed. Please configure guarded folders and/or exclude unnecessary ones, and then click ‘Start backup!’ button", L"", MB_OK|MB_ICONINFORMATION);
    return 0;
}

void scan_enum_files_recursively(DirEntry &de)
{
    de.scan_started = true;
    de.not_traversed = false;

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile((de.full_dir_name() / L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    DirEntry::SubDirs subdirs;

    do
    {
        if (TabBackup::stop_scan) {
            FindClose(h);
            return;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
            continue;

        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_REPARSE_POINT)) // skip hidden files and directories and symbolic links
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (wcscmp(fd.cFileName, L".git") == 0 || wcscmp(fd.cFileName, L"AppData") == 0))
                ASSERT((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
            else
                continue;

        std::wstring file_name(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (file_name == L"." || file_name == L"..")
                continue;
            if (!de.parent && always_excluded_directories.find(file_name) != always_excluded_directories.end())
                continue;

            if (de.subdirs.empty()) { // acquiring of `subdirs_lock` is not necessary here because `treeview_paint()` in other thread just read (i.e. does not write) `subdirs`
                auto it = subdirs.emplace(file_name, DirEntry());
                it.first->second.parent = &de;
                it.first->second.dir_name = &it.first->first;
            }
        }
        else {
            de.dir_files_size += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            de.dir_num_of_files++;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);

    de.size += de.dir_files_size;
    de.num_of_files += de.dir_num_of_files;

    if (de.subdirs.empty()) {
        de.subdirs_lock.acquire();
        de.subdirs = std::move(subdirs);
        de.subdirs_lock.release();
    }

    for (DirEntry *pde = de.parent; pde != nullptr; pde = pde->parent) {
        pde->size += de.size;
        pde->num_of_files += de.num_of_files;
    }

    for (auto &&sd : de.subdirs) {
        if (TabBackup::stop_scan)
            return;
        if (!sd.second.scan_started)
            scan_enum_files_recursively(sd.second);
    }
}

DWORD WINAPI scan_thread_proc(LPVOID p)
{
    scan_enum_files_recursively(*(DirEntry*)p);
    return 0;
}

struct DirItem
{
    const std::wstring *name;
    DirEntry *d;
    int level;
};

void fill_dirs(std::vector<DirItem> &all_dirs, DirEntry &d, int level)
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
int treeview_hover_dir_item_index;
CriticalSection backup_treeview_cs;

void TabBackup::treeview_paint(HDC hdc, int width, int height)
{
    AutoCriticalSection backup_treeview_acs(backup_treeview_cs);

    std::vector<DirItem> dirs;
    for (auto &root_dir_entry : root_dir_entries) {
        DirItem di = {&root_dir_entry->name, &*root_dir_entry, 0};
        dirs.push_back(di);
        if (root_dir_entry->expanded)
            fill_dirs(dirs, *root_dir_entry, 1);
    }

    int new_smax = dirs.size()*LINE_HEIGHT + TREEVIEW_PADDING*2 - 2; // without `- 2` there are artifacts at the bottom of tree view after scrolling to end and scrollbar up button pressed
    int smin, smax;
    ScrollBar_GetRange(scrollbar_wnd, &smin, &smax);
    if (smax != new_smax) {
        ScrollBar_SetRange(scrollbar_wnd, 0, new_smax, FALSE);
        PostMessage(main_wnd, WM_SIZE, 0, 0);
    }

    int scrollpos = ScrollBar_GetPos(scrollbar_wnd);

    // Update hover item
    RECT wnd_rect;
    GetWindowRect(treeview_wnd, &wnd_rect);
    if (!popup_menu_is_open) {
        treeview_hover_dir_item.d = nullptr;
        int item_under_mouse;
        POINT cur_pos;
        GetCursorPos(&cur_pos);
        if (cur_pos.x >= wnd_rect.left
         && cur_pos.y >= wnd_rect.top
         && cur_pos.x < wnd_rect.right
         && cur_pos.y < wnd_rect.bottom
         && (GetAsyncKeyState(VK_LBUTTON) >= 0 || cur_pos == pressed_cur_pos)) { // do not show hover rect when left mouse button is pressed (during scrolling or pressing some button)
            item_under_mouse = (cur_pos.y - wnd_rect.top - TREEVIEW_PADDING + scrollpos) / LINE_HEIGHT;
            if (item_under_mouse < (int)dirs.size()) {
                treeview_hover_dir_item_index = item_under_mouse;
                treeview_hover_dir_item = dirs[item_under_mouse];
            }
        }
    }
    // Draw hover rect
    if (treeview_hover_dir_item.d != nullptr) {
        SelectPenAndBrush spb(hdc, RGB(112, 192, 231), RGB(229, 243, 251)); // colors are taken from Windows Explorer
        Rectangle(hdc, TREEVIEW_PADDING + treeview_hover_dir_item.level * TREEVIEW_LEVEL_OFFSET + ICON_SIZE - 1,
                       TREEVIEW_PADDING - scrollpos +  treeview_hover_dir_item_index    * LINE_HEIGHT, width - TREEVIEW_PADDING,
                       TREEVIEW_PADDING - scrollpos + (treeview_hover_dir_item_index+1) * LINE_HEIGHT);
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
            if (d.d->scan_started || d.d->num_of_files != 0) {
                const int MB = 1024 * 1024;
                //char s[32];
                //sprintf_s(s, d.second->size >= 100*MB ? "%.0f" : d.second->size >= 10*MB ? "%.1f" : d.second->size >= MB ? "%.2f" : "%.3f", d.second->size / double(MB));
                double size_of_files_in_mb = (d.d->num_of_files_excluded == d.d->num_of_files ? d.d->size_excluded : d.d->size - d.d->size_excluded) / double(MB);
                //sprintf_s(s, "%.1f", ((int64_t&)cur_ft - (int64_t&)d.d->max_last_write_time)/(10000000.0*3600*24));
                COLORREF prev_text_color;
                if (d.d->num_of_files_excluded > 0)
                    prev_text_color = SetTextColor(hdc, d.d->num_of_files_excluded == d.d->num_of_files ? RGB(192, 0, 0) : RGB(192, 192, 0));
                DrawTextA(hdc, separate_thousands(size_of_files_in_mb).c_str(), -1, &r, DT_RIGHT);

                r.right = r.left;
                r.left -= FILES_COUNT_COLUMN_WIDTH;
                int num_of_files = d.d->num_of_files_excluded == d.d->num_of_files ? d.d->num_of_files_excluded : d.d->num_of_files - d.d->num_of_files_excluded;
                DrawTextA(hdc, separate_thousands(num_of_files).c_str(), -1, &r, DT_RIGHT);
                if (d.d->num_of_files_excluded > 0)
                    SetTextColor(hdc, prev_text_color);
            }
            else
                DrawText(hdc, L"?", -1, &r, DT_RIGHT);

            r.right = r.left;
            r.left = TREEVIEW_PADDING + d.level * TREEVIEW_LEVEL_OFFSET;
            if (!d.d->subdirs.empty() || d.d->not_traversed)
                DrawIconEx(hdc, r.left, r.top, d.d->expanded ? icon_dir_exp : icon_dir_col, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);

            r.left += ICON_SIZE;
            if (d.d->mode_mixed) {
                //DrawIconEx(hdc, r.left, r.top, mode_mixed_icon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
                static HPEN pen = CreatePen(PS_SOLID, 1, RGB(249, 236, 0));
                SelectPen(hdc, pen);
                SelectBrush(hdc, GetStockBrush(HOLLOW_BRUSH));
                Rectangle(hdc, r.left, r.top, r.left + ICON_SIZE, r.top + ICON_SIZE);
            }
            if (d.d->mode_manual != DirMode::AUTO)
                DrawIconEx(hdc, r.left, r.top, mode_manual_icon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
            DirMode mode = d.d->mode_no_ifp();
            if (mode != DirMode::INHERIT_FROM_PARENT)
                DrawIconEx(hdc, r.left, r.top, mode_icons[(int)mode], ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);

            if (d.d->priority() != DIR_PRIORITY_NORMAL || d.d->priority_manual != DIR_PRIORITY_AUTO) {
                r.left += ICON_SIZE;
                if (d.d->priority_manual != DIR_PRIORITY_AUTO) {
                    //DrawIconEx(hdc, r.left, r.top, mode_manual_icon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
                    static HPEN pen = CreatePen(PS_SOLID, 1, RGB(128, 128, 255));
                    SelectPen(hdc, pen);
                    SelectBrush(hdc, GetStockBrush(HOLLOW_BRUSH));
                    Rectangle(hdc, r.left, r.top, r.left + ICON_SIZE, r.top + ICON_SIZE);
                }
                if (d.d->priority() != DIR_PRIORITY_NORMAL)
                    DrawIconEx(hdc, r.left, r.top, priority_icons[(d.d->priority() > 0 ? 2 : 1) - (int)d.d->priority()], ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
            }

            r.left += ICON_SIZE + LINE_PADDING_LEFT;
            DrawText(hdc, d.name->c_str(), -1, &r, DT_END_ELLIPSIS);
        }

        r.top += LINE_HEIGHT;
    }

    if (treeview_hover_dir_item.d != nullptr
     && treeview_hover_dir_item.d->num_of_files_excluded > 0
     && treeview_hover_dir_item.d->num_of_files_excluded != treeview_hover_dir_item.d->num_of_files) {
        int i = treeview_hover_dir_item_index + 1, m = 1, p = 0;
        SelectPenAndBrush spb(hdc, RGB(112, 192, 231), RGB(229, 243, 251)); // colors are taken from Windows Explorer
        if (TREEVIEW_PADDING - scrollpos + (i+1) * LINE_HEIGHT >= wnd_rect.bottom - wnd_rect.top) { // with `>` bottom outline can disappear (when trying to see this[‘difference between `>` and `>=`’] be aware that single pixel movement of scrollbar thumb can lead to change of `scrollpos` by 2)
            i = treeview_hover_dir_item_index - 1;
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
        SetTextColor(hdc, RGB(192, 0, 0));
        DrawTextA(hdc, separate_thousands(treeview_hover_dir_item.d->size_excluded / double(1024*1024)).c_str(), -1, &r, DT_RIGHT);

        r.right = r.left;
        r.left -= FILES_COUNT_COLUMN_WIDTH;
        DrawTextA(hdc, separate_thousands(treeview_hover_dir_item.d->num_of_files_excluded).c_str(), -1, &r, DT_RIGHT);
    }
}

void TabBackup::treeview_lbdown()
{
    GetCursorPos(&pressed_cur_pos);
    if (treeview_hover_dir_item.d == nullptr)
        return;

    treeview_hover_dir_item.d->expanded = !treeview_hover_dir_item.d->expanded;
    if (treeview_hover_dir_item.d->not_traversed) {
        treeview_hover_dir_item.d->not_traversed = false;

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile((treeview_hover_dir_item.d->full_dir_name() / L"*.*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
                continue;

            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_REPARSE_POINT)) // skip hidden files and directories and symbolic links
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (wcscmp(fd.cFileName, L".git") == 0 || wcscmp(fd.cFileName, L"AppData") == 0))
                    ASSERT((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
                else
                    continue;

            std::wstring file_name(fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (file_name == L"." || file_name == L"..")
                    continue;
                if (!treeview_hover_dir_item.d->parent && always_excluded_directories.find(file_name) != always_excluded_directories.end())
                    continue;

                auto it = treeview_hover_dir_item.d->subdirs.emplace(file_name, DirEntry());
                it.first->second.parent = treeview_hover_dir_item.d;
                it.first->second.dir_name = &it.first->first;
                it.first->second.not_traversed = true;
            }
        } while (FindNextFile(h, &fd));

        FindClose(h);
    }
    InvalidateRect(treeview_wnd, NULL, FALSE);
}

void TabBackup::treeview_rbdown()
{
    HMENU menu = LoadMenu(h_instance, MAKEINTRESOURCE(IDR_BACKUP_TAB_CONTEXT_MENU));
    CheckMenuRadioItem(menu, ID_SORTBY_NAME, ID_SORTBY_NAME + (int)SortBy::COUNT - 1, ID_SORTBY_NAME + (int)sort_by, MF_BYCOMMAND);

    for (int i=0; i<_countof(mode_bitmaps); i++)
        SetMenuItemBitmaps(menu, ID_MODE_EXCLUDED + i, MF_BYCOMMAND, mode_bitmaps[i], mode_bitmaps_selected[i]);
    for (int i=0; i<_countof(priority_bitmaps); i++)
        SetMenuItemBitmaps(menu, ID_PRIORITY_ULTRAHIGH + i, MF_BYCOMMAND, priority_bitmaps[i], priority_bitmaps_selected[i]);

    if (treeview_hover_dir_item.d != nullptr) {
        if (treeview_hover_dir_item.d->mode_auto != DirMode::INHERIT_FROM_PARENT)
            SetMenuItemBitmaps(menu, ID_MODE_AUTO, MF_BYCOMMAND, mode_bitmaps[(int)treeview_hover_dir_item.d->mode_auto], mode_bitmaps_selected[(int)treeview_hover_dir_item.d->mode_auto]);

        const wchar_t *dir_modes[] = {L"Excluded", L"Normal", L"Frozen", L"Append only", L"Inherit from parent"};
        ModifyMenu(menu, ID_MODE_AUTO, MF_BYCOMMAND|MF_STRING, ID_MODE_AUTO, (std::wstring(L"Auto [") + dir_modes[(int)treeview_hover_dir_item.d->mode_auto] + L"]").c_str());
        CheckMenuRadioItem(menu, ID_MODE_EXCLUDED, ID_MODE_EXCLUDED + (int)DirMode::COUNT - 1, ID_MODE_EXCLUDED + (int)treeview_hover_dir_item.d->mode_manual, MF_BYCOMMAND);

        SetMenuItemBitmaps(menu, ID_PRIORITY_AUTO, MF_BYCOMMAND, priority_bitmaps[2 - (int)treeview_hover_dir_item.d->priority_auto], priority_bitmaps_selected[2 - (int)treeview_hover_dir_item.d->priority_auto]);

        const wchar_t *dir_priorities[] = {L"Ultra high", L"High", L"Normal", L"Low", L"Ultra low"};
        ModifyMenu(menu, ID_PRIORITY_AUTO, MF_BYCOMMAND|MF_STRING, ID_PRIORITY_AUTO, (std::wstring(L"Auto priority [") + dir_priorities[2 - (int)treeview_hover_dir_item.d->priority_auto] + L"]").c_str());
        CheckMenuRadioItem(menu, ID_PRIORITY_ULTRAHIGH, ID_PRIORITY_AUTO, treeview_hover_dir_item.d->priority_manual == DIR_PRIORITY_AUTO ? ID_PRIORITY_AUTO : ID_PRIORITY_ULTRAHIGH + (2 - (int)treeview_hover_dir_item.d->priority_manual), MF_BYCOMMAND);
    }

    auto check_if_scan_is_running = []() {
        if (WaitForSingleObject(initial_scan_thread, 0) == WAIT_TIMEOUT || (scan_thread != NULL &&
            WaitForSingleObject(        scan_thread, 0) == WAIT_TIMEOUT)) {
            MessageBox(main_wnd, L"You can not set mode and priority during scan!", NULL, MB_OK|MB_ICONERROR);
            return false;
        }
        return true;
    };

    HMENU sub_menu = GetSubMenu(menu, 0); // this is necessary (see [http://rsdn.org/article/qna/ui/mnuerr1.xml <- http://rsdn.org/forum/winapi/140595.flat <- google:‘TrackPopupMenu "view as popup"’])
    POINT curpos;
    GetCursorPos(&curpos);

    popup_menu_is_open = true;
    auto r = TrackPopupMenu(sub_menu, TPM_LEFTALIGN|TPM_TOPALIGN|TPM_RIGHTBUTTON|TPM_NONOTIFY|TPM_RETURNCMD, curpos.x, curpos.y, 0, treeview_wnd, NULL);
    if (r != 0)
        if (r < ID_SORTBY_NAME + (int)SortBy::COUNT)
            sort_by = SortBy(r - ID_SORTBY_NAME);
        else if (r < ID_MODE_EXCLUDED + (int)DirMode::COUNT) {
            if (check_if_scan_is_running() && treeview_hover_dir_item.d != nullptr) {
                DirMode new_mode = DirMode(r - ID_MODE_EXCLUDED);
                treeview_hover_dir_item.d->set_mode_manual(new_mode);

                if (new_mode != DirMode::AUTO) {
                    std::vector<DirEntry*> manual_distinct;
                    std::function<void(DirEntry&)> find_manual_distinct = [&find_manual_distinct, new_mode, &manual_distinct](DirEntry &de) {
                        for (auto &&sd : de.subdirs) {
                            if (sd.second.mode_manual != DirMode::AUTO) {
                                if (new_mode == DirMode::INHERIT_FROM_PARENT) {
                                    if (sd.second.mode_manual != DirMode::INHERIT_FROM_PARENT)
                                        manual_distinct.push_back(&sd.second);
                                }
                                else
                                    if (sd.second.mode_manual != new_mode && sd.second.mode_manual != DirMode::INHERIT_FROM_PARENT)
                                        manual_distinct.push_back(&sd.second);
                            }
                            find_manual_distinct(sd.second);
                        }
                    };
                    find_manual_distinct(*treeview_hover_dir_item.d);
                    if (!manual_distinct.empty())
                        if (MessageBox(main_wnd, manual_distinct.size() > 1 ? (L"There are " + int_to_str(manual_distinct.size()) + L" sub-entries which manual mode doesn't match up with new mode.\nWould you like to switch their mode also?").c_str() :
                                                                                                                            L"There is 1 sub-entry which manual mode doesn't match up with new mode.\nWould you like to switch its mode also?", L"", MB_YESNO) == IDYES)
                            for (auto &&de : manual_distinct)
                                if (new_mode != DirMode::INHERIT_FROM_PARENT) {
                                    // 1. Try auto
                                    DirMode original_mode_manual = de->mode_manual;
                                    de->mode_manual = DirMode::AUTO;
                                    if (de->mode_no_ifp() == new_mode) {
                                        de->mode_manual = original_mode_manual;
                                        de->set_mode_manual(DirMode::AUTO);
                                    }
                                    else {
                                        // 2. Try inherit from parent
                                        de->mode_manual = DirMode::INHERIT_FROM_PARENT;
                                        if (de->mode_no_ifp() == new_mode) {
                                            de->mode_manual = original_mode_manual;
                                            de->set_mode_manual(DirMode::INHERIT_FROM_PARENT);
                                        }
                                        else {
                                            // 3. Nothing more to do except explicitly/forcibly set new mode
                                            de->mode_manual = original_mode_manual;
                                            de->set_mode_manual(new_mode);
                                        }
                                    }
                                }
                                else {
                                    if (de->mode_auto == DirMode::INHERIT_FROM_PARENT)
                                        de->set_mode_manual(DirMode::AUTO);
                                    else
                                        de->set_mode_manual(DirMode::INHERIT_FROM_PARENT);
                                }

                    std::vector<DirEntry*> auto_distinct;
                    std::function<void(DirEntry&)> find_auto_distinct = [&find_auto_distinct, new_mode, &auto_distinct](DirEntry &de) {
                        for (auto &&sd : de.subdirs) {
                            if (sd.second.mode_manual == DirMode::AUTO) {
                                if (new_mode == DirMode::INHERIT_FROM_PARENT) {
                                    if (sd.second.mode_auto != DirMode::INHERIT_FROM_PARENT)
                                        auto_distinct.push_back(&sd.second);
                                }
                                else
                                    if (sd.second.mode_auto != new_mode && sd.second.mode_auto != DirMode::INHERIT_FROM_PARENT)
                                        auto_distinct.push_back(&sd.second);
                            }
                            find_auto_distinct(sd.second);
                        }
                    };
                    find_auto_distinct(*treeview_hover_dir_item.d);
                    if (!auto_distinct.empty())
                        if (MessageBox(main_wnd, auto_distinct.size() > 1 ? (L"There are " + int_to_str(auto_distinct.size()) + L" sub-entries which automatic mode doesn't match up with new mode.\nWould you like to switch their mode also?").c_str() :
                                                                                                                        L"There is 1 sub-entry which automatic mode doesn't match up with new mode.\nWould you like to switch its mode also?", L"", MB_YESNO) == IDYES)
                            for (auto &&de : auto_distinct)
                                de->set_mode_manual(DirMode::INHERIT_FROM_PARENT);
                }
                else {
                    std::vector<DirEntry*> non_auto;
                    std::function<void(DirEntry&)> find_non_auto = [&find_non_auto, new_mode, &non_auto](DirEntry &de) {
                        for (auto &&sd : de.subdirs) {
                            if (sd.second.mode_manual != DirMode::AUTO)
                                non_auto.push_back(&sd.second);
                            find_non_auto(sd.second);
                        }
                    };
                    find_non_auto(*treeview_hover_dir_item.d);
                    if (!non_auto.empty())
                        if (MessageBox(main_wnd, non_auto.size() > 1 ? (L"There are " + int_to_str(non_auto.size()) + L" sub-entries which mode is not auto.\nWould you like to switch their mode also?").c_str() :
                                                                                                              L"There is 1 sub-entry which mode is not auto.\nWould you like to switch its mode also?", L"", MB_YESNO) == IDYES)
                            for (auto &&de : non_auto)
                                de->set_mode_manual(DirMode::AUTO);
                }

                if (treeview_hover_dir_item.d->mode_no_ifp() != DirMode::EXCLUDED && !treeview_hover_dir_item.d->scan_started) {
                    if (scan_thread != NULL)
                        CloseHandle(scan_thread);
                    TabBackup::stop_scan = false;
                    scan_thread = CreateThread(NULL, 0, scan_thread_proc, treeview_hover_dir_item.d, 0, NULL);
                }
            }
        }
        else if (r <= ID_PRIORITY_AUTO) {
            if (check_if_scan_is_running() && treeview_hover_dir_item.d != nullptr) {
                float priorities[] = {DIR_PRIORITY_ULTRA_HIGH, DIR_PRIORITY_HIGH, DIR_PRIORITY_NORMAL, DIR_PRIORITY_LOW, DIR_PRIORITY_ULTRA_LOW, DIR_PRIORITY_AUTO};
                float new_priority = priorities[r - ID_PRIORITY_ULTRAHIGH];
                treeview_hover_dir_item.d->priority_manual = new_priority;

                if (new_priority == DIR_PRIORITY_AUTO) {
                    std::function<void(DirEntry&)> set_priority_to_auto = [&set_priority_to_auto](DirEntry &de) {
                        for (auto &&sd : de.subdirs) {
                            sd.second.priority_manual = DIR_PRIORITY_AUTO;
                            set_priority_to_auto(sd.second);
                        }
                    };
                    set_priority_to_auto(*treeview_hover_dir_item.d);
                }
                else {
                    std::function<void(DirEntry&)> set_priority = [&set_priority, new_priority](DirEntry &de) {
                        for (auto &&sd : de.subdirs) {
                            if (sd.second.priority_manual == DIR_PRIORITY_AUTO) {
                                if (sd.second.priority_auto != new_priority && sd.second.priority_auto != DIR_PRIORITY_NORMAL)
                                    sd.second.priority_manual = new_priority;
                            }
                            else {
                                if (sd.second.priority_auto == DIR_PRIORITY_NORMAL || sd.second.priority_auto == new_priority)
                                    sd.second.priority_manual = DIR_PRIORITY_AUTO;
                                else
                                    sd.second.priority_manual = new_priority;
                            }
                            set_priority(sd.second);
                        }
                    };
                    set_priority(*treeview_hover_dir_item.d);
                }
            }
        }
        else
            ERROR;
    popup_menu_is_open = false;
    treeview_hover_dir_item.d = nullptr; // to prevent expanding hover item when clicking outside of context menu in order to just close it

    DestroyMenu(menu);
}

void cancel_scan()
{
    backup_treeview_cs.enter();
    for (auto &root_dir_entry : root_dir_entries) {
        root_dir_entry = std::make_unique<RootDirEntry>(root_dir_entry->path, root_dir_entry->name);
        root_dir_entry->mode_auto = DirMode::EXCLUDED;
        root_dir_entry->not_traversed = true;
    }
    treeview_hover_dir_item.d = nullptr;
    backup_treeview_cs.leave();

    backup_state = BackupState::SCAN_CANCELLED;
    SendMessage(main_wnd, WM_COMMAND, IDB_TAB_BACKUP, 0); // update backup tab
}

void restart_scan()
{
    //backup_treeview_cs.enter(); // this is not necessary because `restart_scan()` is called in the same thread as `treeview_paint()`
    for (auto &root_dir_entry : root_dir_entries)
        root_dir_entry = std::make_unique<RootDirEntry>(root_dir_entry->path, root_dir_entry->name);
    root_dir_entries[0]->expanded = true;
    treeview_hover_dir_item.d = nullptr;
    //backup_treeview_cs.leave();

    TabBackup::stop_scan = false;
    TabBackup::cancel_scan = false;
    initial_scan_thread = CreateThread(NULL, 0, initial_scan, NULL, 0, NULL);

    backup_state = BackupState::SCAN_STARTED;
    SendMessage(main_wnd, WM_COMMAND, IDB_TAB_BACKUP, 0); // update backup tab
}

char local_backup_drive;

class MonitoredDir
{
    HANDLE read_directory_changes_thread;

public:
    int root_dir_entry_index;
    std::wstring dir_name;
    bool stop = false;

    MonitoredDir(int root_dir_entry_index, const std::wstring &dir_name) : root_dir_entry_index(root_dir_entry_index), dir_name(dir_name)
    {
        DWORD WINAPI read_directory_changes_thread_proc(LPVOID md);
        read_directory_changes_thread = CreateThread(NULL, 0, read_directory_changes_thread_proc, this, 0, NULL);
    }
    ~MonitoredDir()
    {
        ASSERT(stop);
        WaitForSingleObject(read_directory_changes_thread, INFINITE);
    }
};
std::vector<std::unique_ptr<MonitoredDir>> monitored_dirs;

void stop_monitoring()
{
    for (auto &md : monitored_dirs)
        md->stop = true;
    monitored_dirs.clear();
}

struct DirChange
{
    int root_dir_entry_index; // corresponding root_dir_entry can be found by `dir_name`, of course, but storing this index is simpler [and faster]
    std::wstring dir_name;
    enum class Operation
    {
        CREATE,
        MODIFY, // MODIFY is better than CHANGE because ‘modified/modification time’
        RENAME,
        MOVE,
#undef DELETE
        DELETE
    } operation;
    std::wstring fname;
    std::wstring new_fname;
    int time;
};
std::list<DirChange> dir_changes;
SpinLock dir_changes_lock;

void add_dir_change(MonitoredDir *md, DirChange::Operation operation, const std::wstring &fname, const std::wstring &new_fname = std::wstring())
{
    if (operation == DirChange::Operation::MOVE && fname == new_fname)
        return;

    DirChange dc;
    dc.time = timeGetTime();

    dir_changes_lock.acquire();
    if (operation == DirChange::Operation::MODIFY) {
        for (auto &&d : dir_changes)
            if ((d.operation == DirChange::Operation::MODIFY || d.operation == DirChange::Operation::CREATE) && d.fname == fname && d.root_dir_entry_index == md->root_dir_entry_index && d.dir_name == md->dir_name) {
                d.time = dc.time; // just update time
                goto skip_add;
            }
    }
    else if (operation == DirChange::Operation::DELETE) {
        for (auto it = dir_changes.begin(); it != dir_changes.end(); it++)
            if (it->operation == DirChange::Operation::CREATE && it->fname == fname && it->root_dir_entry_index == md->root_dir_entry_index && it->dir_name == md->dir_name) {
                dir_changes.erase(it);
                goto skip_add;
            }
    }
    dc.root_dir_entry_index = md->root_dir_entry_index;
    dc.dir_name = md->dir_name;
    dc.operation = operation;
    dc.fname = fname;
    dc.new_fname = new_fname;
    dir_changes.push_back(std::move(dc));
skip_add:
    dir_changes_lock.release();
}

DWORD WINAPI read_directory_changes_thread_proc(LPVOID md_)
{
    MonitoredDir *md = (MonitoredDir*)md_;
    HANDLE dir_handle = CreateFileW((md->dir_name + L'\\').c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED|FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ASSERT(dir_handle != INVALID_HANDLE_VALUE);

    OVERLAPPED opd;
    opd.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    std::wstring just_removed_file_name;

    for(;;)
    {
        char fni_buf[10000];
        if (!ReadDirectoryChangesW(dir_handle, fni_buf, sizeof(fni_buf), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE, NULL, &opd, NULL))
            ERROR;
        DWORD dwNumberOfBytesTransfered;
        for(;;)
            if (GetOverlappedResult(dir_handle, &opd, &dwNumberOfBytesTransfered, FALSE))
            {
                FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION*)fni_buf, *next_fni;
                if (!just_removed_file_name.empty()) {
                    if (fni->Action == FILE_ACTION_ADDED && path_base_name(just_removed_file_name) == path_base_name(std::wstring(fni->FileName, fni->FileNameLength/sizeof(WCHAR)))) {
                        add_dir_change(md, DirChange::Operation::MOVE, just_removed_file_name, std::wstring(fni->FileName, fni->FileNameLength/sizeof(WCHAR)));
                        just_removed_file_name.clear();
                        if (fni->NextEntryOffset == 0)
                            break;
                        fni = (FILE_NOTIFY_INFORMATION*)((char*)fni + fni->NextEntryOffset);
                    }
                    else {
                        add_dir_change(md, DirChange::Operation::DELETE, just_removed_file_name);
                        just_removed_file_name.clear();
                    }
                }

                for (; ;fni = next_fni)
                {
                    next_fni = (FILE_NOTIFY_INFORMATION*)((char*)fni + fni->NextEntryOffset);
                    std::wstring fname(fni->FileName, fni->FileNameLength/sizeof(WCHAR));
                    DirChange::Operation operation;
                    switch (fni->Action)
                    {
                    case FILE_ACTION_ADDED:
                        if (!just_removed_file_name.empty() && path_base_name(just_removed_file_name) == path_base_name(fname)) {
                            add_dir_change(md, DirChange::Operation::MOVE, just_removed_file_name, fname);
                            just_removed_file_name.clear();
                            goto continue_;
                        }
                        operation = DirChange::Operation::CREATE;
                        break;
                    case FILE_ACTION_REMOVED:
                        if (next_fni->Action == FILE_ACTION_ADDED && path_base_name(fname) == path_base_name(std::wstring(next_fni->FileName, next_fni->FileNameLength/sizeof(WCHAR)))) {
                            add_dir_change(md, DirChange::Operation::MOVE, fname, std::wstring(next_fni->FileName, next_fni->FileNameLength/sizeof(WCHAR)));
                            fni = next_fni;
                            next_fni = (FILE_NOTIFY_INFORMATION*)((char*)fni + fni->NextEntryOffset);
                            goto continue_;
                        }
                        else {
                            if (!just_removed_file_name.empty()) {
                                add_dir_change(md, DirChange::Operation::DELETE, just_removed_file_name);
                                //just_removed_file_name.clear();
                            }
                            just_removed_file_name = fname;
                            goto continue_;
                        }
                        break;
                    case FILE_ACTION_MODIFIED:
                        if (GetFileAttributes((md->dir_name / fname).c_str()) & FILE_ATTRIBUTE_DIRECTORY) // skip this action because there are excess modify directory notifications
                            goto continue_;
                        operation = DirChange::Operation::MODIFY;
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        if (next_fni->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                            add_dir_change(md, DirChange::Operation::RENAME, fname, std::wstring(next_fni->FileName, next_fni->FileNameLength/sizeof(WCHAR)));
                            fni = next_fni;
                            next_fni = (FILE_NOTIFY_INFORMATION*)((char*)fni + fni->NextEntryOffset);
                            goto continue_;
                        }
                        else
                            ERROR;
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        ERROR;
                        break;
                    }
                    add_dir_change(md, operation, fname);
continue_:
                    if (fni->NextEntryOffset == 0) break;
                }
                break;
            } else {
                if (!just_removed_file_name.empty()) {
                    add_dir_change(md, DirChange::Operation::DELETE, just_removed_file_name);
                    just_removed_file_name.clear();
                }
                if (md->stop)
                    goto break_;
                Sleep(100);
            }
    }
break_:

    CloseHandle(opd.hEvent);
    CloseHandle(dir_handle);
    return 0;
}

HANDLE apply_directory_changes_thread;
bool stop_apply_directory_changes_thread = false;
DWORD WINAPI apply_directory_changes_thread_proc(LPVOID md)
{
    while (!stop_apply_directory_changes_thread) {
        int time = timeGetTime();
        std::list<DirChange> tdir_changes;

        dir_changes_lock.acquire();
        for (auto it = dir_changes.begin(); it != dir_changes.end();) {
            if ((it->operation == DirChange::Operation::MODIFY || it->operation == DirChange::Operation::CREATE) && time - it->time < 500) {
                ++it;
                continue;
            }
            tdir_changes.push_back(DirChange());
            std::swap(tdir_changes.back(), *it);
            it = dir_changes.erase(it);
        }
        dir_changes_lock.release();

        for (auto &&dc : tdir_changes) {
            wchar_t s[30+MAX_PATH*2], *ops[] = {L"CREATE", L"MODIFY", L"RENAME", L"MOVE", L"DELETE"};
            int n = wsprintf(s, L"%s %s", ops[(int)dc.operation], dc.fname.c_str());
            if (!dc.new_fname.empty())
                wsprintf(s + n, L" -> %s\n", dc.new_fname.c_str());
            else
                s[n] = L'\n', s[n+1] = 0;
            OutputDebugString(s);
        }

        Sleep(250);
    }

    return 0;
}

void collect_monitored_dirs(int rdei, const std::wstring &dir_name, DirEntry &de)
{
    DirMode mode = de.mode_no_ifp();
    ASSERT(mode != DirMode::INHERIT_FROM_PARENT);

    if (mode == DirMode::EXCLUDED) {
        if (de.mode_mixed) // may be there are some non-excluded subdirectories
            for (auto &&sd : de.subdirs)
                collect_monitored_dirs(rdei, dir_name / sd.first, sd.second);
    }
    else
        monitored_dirs.push_back(std::make_unique<MonitoredDir>(rdei, dir_name));
}

INT_PTR CALLBACK backup_drive_selection_dlg_proc(HWND dlg_wnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    static std::vector<std::unique_ptr<Button>> buttons;

    switch (message)
    {
    case WM_INITDIALOG: {
        HWND drives_list = GetDlgItem(dlg_wnd, IDC_DRIVES_LIST);
        DWORD drives = GetLogicalDrives();
        for (int i=0; i<26; i++)
            if (drives & (1 << i)) {
                std::wstring drive_letter_str(1, L'A' + i);

                wchar_t device_name[255] = L"\0";
                QueryDosDevice((drive_letter_str + L':').c_str(), device_name, _countof(device_name)); // [http://delphi-hlp.ru/index.php/rabota-s-zhelezom/diski.html?start=11 <- google:‘GetDiskFreeSpaceEx for "disk a"’]
                if (std::wstring(device_name).find(L"\\Device\\Floppy") == 0)
                    continue; // skip floppy drives; this is needed to suppress error message box:
// ---------------------------
// Windows - Устройство не готово
// ---------------------------
// Exception Processing Message c00000a3 Parameters 75b3bf7c 4 75b3bf7c 75b3bf7c
// ---------------------------
// Отмена   Повторить   Продолжить
// ---------------------------
                // \\ Another solution is to call `SetErrorMode(SEM_FAILCRITICALERRORS)` ([https://forum.sources.ru/index.php?showtopic=83601 <- google:‘GetDiskFreeSpace for floppy’]),
                // \\ but there will still be a few seconds delay in `GetDiskFreeSpaceEx()` call for floppy drives

                ULARGE_INTEGER free_bytes_available_to_caller;
                if (!GetDiskFreeSpaceEx((drive_letter_str + L":\\").c_str(), &free_bytes_available_to_caller, NULL, NULL))
                    continue;
                if (free_bytes_available_to_caller.QuadPart == 0)
                    continue;

                wchar_t label[MAX_PATH+1] = L"\0";
                GetVolumeInformation((drive_letter_str + L":\\").c_str(), label, _countof(label), NULL, NULL, NULL, NULL, 0);
                ListBox_SetItemData(drives_list, ListBox_AddString(drives_list, (
                    drive_letter_str + (label[0] ? std::wstring(L" [") + label + L"]" : L"") + L" ("
                    + int64_to_str(free_bytes_available_to_caller.QuadPart / (1024*1024*1024)) + L" GiB free)").c_str()), i);
            }

        buttons.push_back(std::make_unique<Button>(dlg_wnd, IDOK));
        buttons.push_back(std::make_unique<Button>(dlg_wnd, IDCANCEL));

        return TRUE; }

    case WM_DRAWITEM:
        InvalidateRect(((DRAWITEMSTRUCT*)lparam)->hwndItem, NULL, TRUE); // needed for correct visual switching to/from PRESSED state
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDOK: {
            HWND drives_list = GetDlgItem(dlg_wnd, IDC_DRIVES_LIST);
            if (ListBox_GetCurSel(drives_list) == LB_ERR) {
                MessageBox(dlg_wnd, L"Please select a drive for storing local backup", NULL, MB_OK|MB_ICONEXCLAMATION);
                break;
            }
            int selected_drive = ListBox_GetItemData(drives_list, ListBox_GetCurSel(drives_list));
            ULARGE_INTEGER free_bytes_available_to_caller;
            ASSERT(GetDiskFreeSpaceEx((std::wstring(1, L'A' + selected_drive) + L":\\").c_str(), &free_bytes_available_to_caller, NULL, NULL));
            uint64_t total_size = 0;
            for (const auto &root_dir_entry : root_dir_entries)
                total_size += root_dir_entry->size - root_dir_entry->size_excluded;
            if (total_size * 125 / 100 > free_bytes_available_to_caller.QuadPart) {
                MessageBox(dlg_wnd, replace_all(L"There is not enough free space on drive <drive_letter>.\nPlease select another drive.", L"<drive_letter>", std::wstring(1, L'A' + selected_drive)).c_str(), NULL, MB_OK|MB_ICONSTOP);
                break;
            }

            local_backup_drive = 'A' + selected_drive;
            backup_state = BackupState::BACKUP_STARTED;
            for (size_t i=0; i<root_dir_entries.size(); i++)
                collect_monitored_dirs(i, root_dir_entries[i]->path, *root_dir_entries[i]);
            apply_directory_changes_thread = CreateThread(NULL, 0, apply_directory_changes_thread_proc, NULL, 0, NULL);
            SendMessage(main_wnd, WM_COMMAND, IDB_TAB_PROGRESS, 0); }
        case IDCANCEL:
            buttons.clear();
            EndDialog(dlg_wnd, LOWORD(wparam));
            return TRUE;
        }
        break;
    }

    return FALSE;
}
