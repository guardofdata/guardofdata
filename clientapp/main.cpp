#include "precompiled.h"
#include "resource.h"
#include "tabs.h"

#pragma comment (lib, "winmm.lib")

const UINT WM_TRAY = WM_USER;
#define RECTARGS(rect) (rect).left, (rect).top, (rect).right-(rect).left, (rect).bottom-(rect).top

HINSTANCE h_instance;
HWND main_wnd, treeview_wnd, scrollbar_wnd;
int scrollbar_width;
HICON icon_dir_col, icon_dir_exp;
BackupState backup_state = BackupState::SCAN_STARTED;
std::vector<std::unique_ptr<Button>> tab_buttons;
std::unique_ptr<Tab> current_tab;

void error_fn(const char *file, int line)
{
    char s[300];
    sprintf_s(s, "Error at file %s (%i)", file, line);
    MessageBoxA(IsWindow(main_wnd) ? main_wnd : NULL, s, NULL, MB_OK);
}

void assertion_failed(const char *file, int line)
{
    char s[300];
    sprintf_s(s, "Assertion failed!\nFile: %s\nLine: %i", file, line);
    MessageBoxA(IsWindow(main_wnd) ? main_wnd : NULL, s, NULL, MB_OK);
}

struct TV_SB_WndRects
{
    RECT treeview_wnd_rect, scrollbar_wnd_rect;
};
TV_SB_WndRects calc_treeview_and_scrollbar_wnd_rects()
{
    RECT cr;
    GetClientRect(main_wnd, &cr);
    RECT tr = {
        mul_by_system_scaling_factor(10),
        mul_by_system_scaling_factor(10 + TAB_BUTTON_HEIGHT + 10 + current_tab->treeview_offsety()),
        cr.right  - mul_by_system_scaling_factor(10) - scrollbar_width,
        cr.bottom - mul_by_system_scaling_factor(10),
    };
    RECT sr = {
        tr.right,
        tr.top,
        tr.right + scrollbar_width,
        tr.bottom
    };
    TV_SB_WndRects r = {tr, sr};
    return r;
}

LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_CREATE:
        // [https://www.codeproject.com/Articles/7469/Quick-code-page-converter <- https://stackoverflow.com/questions/13146607/how-to-make-a-taskbar-system-tray-application-in-windows <- google:‘site:stackoverflow.com system tray application c++’]
        {
            NOTIFYICONDATA stData = {0};
            stData.cbSize = sizeof(stData);
            stData.hWnd = hwnd;
            stData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            stData.uCallbackMessage = WM_TRAY;
            stData.hIcon = LoadIcon(h_instance, MAKEINTRESOURCE(IDI_CLIENTAPP));
            wcscpy_s(stData.szTip, L"Guard of Data");
            if (!Shell_NotifyIcon(NIM_ADD, &stData)) {
                ERROR;
                return -1;
            }
        }
        return 0;

    case WM_DESTROY:
        {
            NOTIFYICONDATA stData = {0};
            stData.cbSize = sizeof(stData);
            stData.hWnd = hwnd;
            if (!Shell_NotifyIcon(NIM_DELETE, &stData))
                ERROR;
        }
        PostQuitMessage(0);
        return 0;

    case WM_CLOSE:
        if (wcsstr(GetCommandLine(), L" --exit-on-close-button"))
            break;
        ShowWindow(hwnd, SW_HIDE);
        return 0; // to not process message by the system, which call DestroyWindow in DefWindowProc

    case WM_TRAY:
        switch (lparam)
        {
        case WM_LBUTTONDBLCLK:
            ShowWindow(hwnd, SW_NORMAL);
            SetForegroundWindow(hwnd);
            break;

        case WM_RBUTTONDOWN:
            {
                HMENU menu = LoadMenu(h_instance, MAKEINTRESOURCE(IDR_TRAY_POPUP_MENU));
                HMENU sub_menu = GetSubMenu(menu, 0);
                POINT curpos;
                GetCursorPos(&curpos);
                SetForegroundWindow(hwnd); // without this line of code menu will not disappear when the user clicks outside of the menu ([https://stackoverflow.com/questions/15494591/hide-close-menu-when-mouse-is-clicked-outside-its-focus <- google:‘TrackPopupMenu click does not close it site:stackoverflow.com’]:‘To display a context menu for a notification icon, the current window must be the foreground window before the application calls TrackPopupMenu’)
                switch (TrackPopupMenu(sub_menu, TPM_LEFTALIGN|TPM_BOTTOMALIGN|TPM_RIGHTBUTTON|TPM_NONOTIFY|TPM_RETURNCMD, curpos.x, curpos.y, 0, hwnd, NULL))
                {
                case IDM_SHOWMAINWINDOW:
                    ShowWindow(hwnd, SW_NORMAL);
                    SetForegroundWindow(hwnd);
                    break;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    break;
                }
                DestroyMenu(menu);
            }
            break;
        }
        return 0;

    case WM_DRAWITEM:
        {
        DRAWITEMSTRUCT *pdis = (DRAWITEMSTRUCT*)lparam;
        //((Button*)GetWindowLongPtr(pdis->hwndItem, GWL_USERDATA))->dis_selected = (pdis->itemState & ODS_SELECTED) != 0;
        InvalidateRect(pdis->hwndItem, NULL, TRUE); // needed for correct visual switching to/from PRESSED state
        return TRUE;
        }

    case WM_COMMAND:
        switch (WORD cid = LOWORD(wparam))
        {
        case IDB_TAB_BACKUP:
        case IDB_TAB_PROGRESS:
        case IDB_TAB_LOG:
            for (auto &&tab_button : tab_buttons)
                tab_button->select(tab_button->id == cid);
            switch (cid) {
            case IDB_TAB_BACKUP:   switch_tab(std::make_unique<TabBackup  >()); break;
            case IDB_TAB_PROGRESS: switch_tab(std::make_unique<TabProgress>()); break;
            case IDB_TAB_LOG:      switch_tab(std::make_unique<TabLog     >()); break;
            }
            PostMessage(hwnd, WM_SIZE, 0, 0);
            break;

        case IDB_START_BACKUP:
            DialogBox(h_instance, MAKEINTRESOURCE(IDD_BACKUP_DRIVE_SELECTION), hwnd, backup_drive_selection_dlg_proc);
            break;
        }
        return 0;

    case WM_SIZING: {
        const int MIN_WIDTH  = mul_by_system_scaling_factor(550),
                  MIN_HEIGHT = mul_by_system_scaling_factor(300);
        LPRECT lrect = (LPRECT)lparam;

        if (lrect->right - lrect->left < MIN_WIDTH)
            switch (wparam)
            {
            case WMSZ_RIGHT:
            case WMSZ_TOPRIGHT:
            case WMSZ_BOTTOMRIGHT:
                lrect->right = lrect->left + MIN_WIDTH;
                break;
            case WMSZ_LEFT:
            case WMSZ_TOPLEFT:
            case WMSZ_BOTTOMLEFT:
                lrect->left = lrect->right - MIN_WIDTH;
                break;
            }

        if (lrect->bottom - lrect->top < MIN_HEIGHT)
            switch (wparam)
            {
            case WMSZ_BOTTOM:
            case WMSZ_BOTTOMLEFT:
            case WMSZ_BOTTOMRIGHT:
                lrect->bottom = lrect->top + MIN_HEIGHT;
                break;
            case WMSZ_TOP:
            case WMSZ_TOPLEFT:
            case WMSZ_TOPRIGHT:
                lrect->top = lrect->bottom - MIN_HEIGHT;
                break;
            }
        return TRUE; }

    case WM_SIZE: {
        auto wr = calc_treeview_and_scrollbar_wnd_rects();
        MoveWindow(treeview_wnd, RECTARGS(wr.treeview_wnd_rect), TRUE);
        MoveWindow(scrollbar_wnd, RECTARGS(wr.scrollbar_wnd_rect), TRUE);
        InvalidateRect(treeview_wnd, NULL, TRUE);
        SCROLLINFO si = {sizeof(si)};
        si.fMask = SIF_PAGE;
        si.nPage = wr.treeview_wnd_rect.bottom - wr.treeview_wnd_rect.top;
        int smin, smax;
        ScrollBar_GetRange(scrollbar_wnd, &smin, &smax);
        if (smax < (int)si.nPage) {
            EnableWindow(scrollbar_wnd, FALSE);
            ScrollBar_SetPos(scrollbar_wnd, 0, FALSE);
        }
        else {
            SetScrollInfo(scrollbar_wnd, SB_CTL, &si, TRUE);
            EnableWindow(scrollbar_wnd, TRUE);
            InvalidateRect(scrollbar_wnd, NULL, TRUE);
        }
        break; }

    case WM_VSCROLL:
        switch (LOWORD(wparam))
        {
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si = {sizeof(si), SIF_TRACKPOS};
            GetScrollInfo(scrollbar_wnd, SB_CTL, &si);
            ScrollBar_SetPos(scrollbar_wnd, si.nTrackPos, TRUE);
            break; }
        case SB_LINEDOWN:
            ScrollBar_SetPos(scrollbar_wnd, ScrollBar_GetPos(scrollbar_wnd) + LINE_HEIGHT, TRUE);
            break;
        case SB_LINEUP:
            ScrollBar_SetPos(scrollbar_wnd, ScrollBar_GetPos(scrollbar_wnd) - LINE_HEIGHT, TRUE);
            break;
        case SB_PAGEDOWN:
        case SB_PAGEUP:
            SCROLLINFO si = {sizeof(si), SIF_PAGE};
            GetScrollInfo(scrollbar_wnd, SB_CTL, &si);
            switch (LOWORD(wparam))
            {
            case SB_PAGEDOWN:
                ScrollBar_SetPos(scrollbar_wnd, ScrollBar_GetPos(scrollbar_wnd) + si.nPage/2, TRUE);
                break;
            case SB_PAGEUP:
                ScrollBar_SetPos(scrollbar_wnd, ScrollBar_GetPos(scrollbar_wnd) - si.nPage/2, TRUE);
                break;
            }
            break;
        }

        InvalidateRect(treeview_wnd, NULL, TRUE);
        return 0;

    case WM_MOUSEWHEEL:
        if (IsWindowEnabled(scrollbar_wnd))
            for (int i=0; i<3; i++)
                SendMessage(main_wnd, WM_VSCROLL, MAKEWPARAM(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? SB_LINEUP : SB_LINEDOWN, 0),0);
        break;
    }

    return DefWindowProc(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK treeview_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_PAINT:
        PAINTSTRUCT ps;
        RECT r;
        GetClientRect(hwnd, &r);
        {
        DoubleBufferedDC hdc(BeginPaint(hwnd, &ps), r.right, r.bottom);

        //{SelectPenAndBrush spb(hdc, RGB(223, 223, 223), RGB(255, 255, 255)); // commented out as there were artifacts (white background from dir_collapsed.ico) at the bottom of tree view after scrolling to end and scrollbar up button pressed
        //Rectangle(hdc, 0, 0, r.right, r.bottom);}
        FillRect(hdc, &r, GetStockBrush(WHITE_BRUSH));

        current_tab->treeview_paint(hdc, r.right, r.bottom);

        static HPEN pen = CreatePen(PS_SOLID, 1, RGB(223, 223, 223));
        SelectPen(hdc, pen);
        SelectBrush(hdc, GetStockBrush(HOLLOW_BRUSH));
        Rectangle(hdc, 0, 0, r.right, r.bottom);
        }
        EndPaint(hwnd, &ps);
        return 0;

    case WM_MOUSEMOVE:
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_LBUTTONDOWN:
        current_tab->treeview_lbdown();
        break;

    case WM_RBUTTONDOWN:
        current_tab->treeview_rbdown();
        break;
    }

    return DefWindowProc(hwnd, message, wparam, lparam);
}

void downscale_bitmap(HDC hdc_screen, HBITMAP *bitmap)
{
    BITMAP src_bmp;
    GetObject(*bitmap, sizeof(BITMAP), &src_bmp);
    int src_width  = src_bmp.bmWidth,
        src_height = src_bmp.bmHeight;

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = src_width;
    bi.biHeight = -src_height; // inspired by [https://www.codeproject.com/Tips/150253/Create-bitmap-from-pixels <- google:‘winapi create bitmap from pixels’]
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    std::vector<uint32_t> sbits(src_width * src_height);
    GetDIBits(hdc_screen, *bitmap, 0, src_height, sbits.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    int res_width  = GetSystemMetrics(SM_CXMENUCHECK),
        res_height = GetSystemMetrics(SM_CYMENUCHECK);
    std::vector<uint32_t> dbits(res_width * res_height);

    extern void area_averaging_image_scale(uint32_t *dst, int dst_width, int dst_height, const uint32_t *src, int src_width, int src_height);
    area_averaging_image_scale(dbits.data(), res_width, res_height, sbits.data(), src_width, src_height);

    *bitmap = CreateBitmap(res_width, res_height, 1, 32, dbits.data()); // [https://stackoverflow.com/a/55895127/2692494 <- google:‘winapi create bitmap from pixels’]
}

const int ICO_RES_SIZE = 64;

// [https://www.gamedev.net/forums/topic/617849-win32-draw-to-bitmap/ <- google:‘winapi draw into bitmap’]
void create_menu_item_bitmaps_from_icon(HICON icon, HICON checked_background, HBITMAP *bitmap_unchecked, HBITMAP *bitmap_checked)
{
    HDC hdc_screen = GetDC(NULL);
    HDC hdc_bmp = CreateCompatibleDC(hdc_screen);
    int width  = ICO_RES_SIZE,
        height = ICO_RES_SIZE;

    // Create unchecked bitmap
    *bitmap_unchecked = CreateCompatibleBitmap(hdc_screen, width, height);
    HBITMAP hbm_old = (HBITMAP)SelectObject(hdc_bmp, *bitmap_unchecked);

    // Draw unchecked bitmap
    RECT r = {0, 0, width, height};
    FillRect(hdc_bmp, &r, GetSysColorBrush(COLOR_MENU));
    DrawIconEx(hdc_bmp, 0, 0, icon, width, height, 0, NULL, DI_NORMAL);
    downscale_bitmap(hdc_screen, bitmap_unchecked);

    // Create checked bitmap
    *bitmap_checked = CreateCompatibleBitmap(hdc_screen, width, height);
    SelectObject(hdc_bmp, *bitmap_checked);

    // Draw checked bitmap
    DrawIconEx(hdc_bmp, 0, 0, checked_background, width, height, 0, NULL, DI_NORMAL);
    DrawIconEx(hdc_bmp, 0, 0, icon, width, height, 0, NULL, DI_NORMAL);
    downscale_bitmap(hdc_screen, bitmap_checked);

    // Clean up the GDI objects we've created
    SelectObject(hdc_bmp, hbm_old);
    DeleteDC(hdc_bmp);
    ReleaseDC(NULL, hdc_screen);
}

int APIENTRY _tWinMain(HINSTANCE hInstance,
                       HINSTANCE hPrevInstance,
                       LPTSTR    lpCmdLine,
                       int       nCmdShow)
{
    h_instance = hInstance;

    // Register the main window class
    {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIENTAPP));
    //wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(248, 248, 248)); // color is taken from [https://www.pcmag.com/reviews/spideroak-one] //GetSysColorBrush(COLOR_3DFACE);
    wc.lpszClassName = L"Guard of Data window class";

    main_wnd = FindWindow(wc.lpszClassName, NULL);
    if (main_wnd) {
        PostMessage(main_wnd, WM_TRAY, 0, WM_LBUTTONDBLCLK);
        return 0;
    }

    if (!RegisterClass(&wc)) ERROR;

    RECT wnd_rect = {100, 100, 800+100, 600+100};
    main_wnd = CreateWindowEx(0, wc.lpszClassName, L"Guard of Data", WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN, RECTARGS(wnd_rect), NULL, NULL, hInstance, 0);
    if (!main_wnd) ERROR;
    }

    LOGFONT lf = {FONT_HEIGHT, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH|FF_SWISS, _T("")};
    button_font = CreateFontIndirect(&lf);
    treeview_font = button_font;
    lf.lfWeight = FW_BOLD;
    button_font_bold = CreateFontIndirect(&lf);

    tab_buttons.push_back(std::make_unique<Button>(IDB_TAB_BACKUP,   L"Backup",   10,                      10, TAB_BUTTON_WIDTH + 1, TAB_BUTTON_HEIGHT));
    tab_buttons.push_back(std::make_unique<Button>(IDB_TAB_PROGRESS, L"Progress", 10 +   TAB_BUTTON_WIDTH, 10, TAB_BUTTON_WIDTH + 1, TAB_BUTTON_HEIGHT));
    tab_buttons.push_back(std::make_unique<Button>(IDB_TAB_LOG,      L"Log",      10 + 2*TAB_BUTTON_WIDTH, 10, TAB_BUTTON_WIDTH + 1, TAB_BUTTON_HEIGHT));
    SendMessage(main_wnd, WM_COMMAND, IDB_TAB_BACKUP, 0);

    auto wr = calc_treeview_and_scrollbar_wnd_rects();
    {// Tree View child window is common between all tabs to avoid flickering during tab switching (when one Tree View is destroyed and another is created)
    WNDCLASS wc = {0};
    wc.lpfnWndProc = treeview_wnd_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"Guard of Data tree view window class";
    if (!RegisterClass(&wc)) ERROR;
    treeview_wnd = CreateWindowEx(0, wc.lpszClassName, L"", WS_CHILD|WS_VISIBLE, RECTARGS(wr.treeview_wnd_rect), main_wnd, NULL, hInstance, 0);
    if (!treeview_wnd) ERROR;
    }

    scrollbar_wnd = CreateWindowEx(0, L"SCROLLBAR", L"", WS_VISIBLE|SBS_VERT|SBS_SIZEBOXBOTTOMRIGHTALIGN|WS_CHILD, RECTARGS(wr.scrollbar_wnd_rect), main_wnd, NULL, hInstance, 0);
    if (!scrollbar_wnd) ERROR;
    GetClientRect(scrollbar_wnd, &wr.scrollbar_wnd_rect);
    scrollbar_width = wr.scrollbar_wnd_rect.right - wr.scrollbar_wnd_rect.left;

    icon_dir_col = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_DIR_COLLAPSED), IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);
    icon_dir_exp = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_DIR_EXPANDED),  IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);
    extern HICON mode_icons[4], mode_mixed_icon, mode_manual_icon, priority_icons[4];
    for (int i=0; i<_countof(mode_icons); i++)
        mode_icons[i] = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_MINUS_RED + i), IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);
    mode_mixed_icon  = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_DOT_YELLOW),   IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);
    mode_manual_icon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_BLUE_CORNERS), IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);
    for (int i=0; i<_countof(priority_icons); i++)
        priority_icons[i] = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_UP_DOUBLE_ARROW_GREEN + i), IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);

    HICON menu_item_selection_icon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_MENU_ITEM_SELECTION), IMAGE_ICON, ICO_RES_SIZE, ICO_RES_SIZE, 0);
    extern HBITMAP mode_bitmaps[4], mode_bitmaps_selected[4];
    for (int i=0; i<_countof(mode_bitmaps); i++)
        create_menu_item_bitmaps_from_icon((HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_MINUS_RED + i), IMAGE_ICON, ICO_RES_SIZE, ICO_RES_SIZE, 0), menu_item_selection_icon, &mode_bitmaps[i], &mode_bitmaps_selected[i]);

    extern HBITMAP priority_bitmaps[5], priority_bitmaps_selected[5];
    for (int i=0; i<_countof(priority_bitmaps); i++)
        if (i != 2)
            create_menu_item_bitmaps_from_icon((HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_UP_DOUBLE_ARROW_GREEN + (i < 2 ? i : i - 1)), IMAGE_ICON, ICO_RES_SIZE, ICO_RES_SIZE, 0), menu_item_selection_icon, &priority_bitmaps[i], &priority_bitmaps_selected[i]);

    if (wcsstr(GetCommandLine(), L" --show-window"))
        ShowWindow(main_wnd, SW_NORMAL);

    DWORD WINAPI initial_scan(LPVOID);
    HANDLE initial_scan_thread = CreateThread(NULL, 0, initial_scan, NULL, 0, NULL);

    SetTimer(treeview_wnd, 1, 200, NULL);

    MSG msg;
    BOOL ret;
    // [https://docs.microsoft.com/en-us/windows/win32/winmsg/using-messages-and-message-queues]
    while ((ret = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (ret == -1) {
            ERROR;
        }
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    TabBackup::stop_scan = true;
    WaitForSingleObject(initial_scan_thread, INFINITE);
    extern bool stop_apply_directory_changes_thread;
    extern HANDLE apply_directory_changes_thread;
    stop_apply_directory_changes_thread = true;
    void stop_monitoring();
    stop_monitoring();
    WaitForSingleObject(apply_directory_changes_thread, INFINITE);

    tab_buttons.clear(); // may be unnecessary
    current_tab.reset(); // may be unnecessary

    return msg.wParam;
}
