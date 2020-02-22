#include "precompiled.h"
#include "resource.h"
#include "tabs.h"

const UINT WM_TRAY = WM_USER;
#define RECTARGS(rect) (rect).left, (rect).top, (rect).right-(rect).left, (rect).bottom-(rect).top

HINSTANCE h_instance;
HWND main_wnd, treeview_wnd, scrollbar_wnd;
int scrollbar_width;
HICON icon_dir_col, icon_dir_exp;
std::vector<std::unique_ptr<Button>> tab_buttons;
std::unique_ptr<Tab> current_tab;

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
        if (smax < (int)si.nPage)
            EnableWindow(scrollbar_wnd, FALSE);
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
    }

    return DefWindowProc(hwnd, message, wparam, lparam);
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
    extern HICON mode_icons[4], mode_mixed_icon;
    int mode_icons_ids[] = {IDI_MINUS_RED, IDI_DOT_GREEN, IDI_SNOWFLAKE, IDI_PLUS_BLUE};
    for (int i=0; i<_countof(mode_icons); i++)
        mode_icons[i] = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(mode_icons_ids[i]), IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);
    mode_mixed_icon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_DOT_YELLOW), IMAGE_ICON, ICON_SIZE, ICON_SIZE, 0);

    if (wcsstr(GetCommandLine(), L" --show-window"))
        ShowWindow(main_wnd, SW_NORMAL);

    DWORD WINAPI initial_scan(LPVOID);
    HANDLE initial_scan_thread = CreateThread(NULL, 0, initial_scan, NULL, 0, NULL);

    SetTimer(treeview_wnd, 1, 200, NULL);

    MSG msg;
    BOOL ret;
    // [https://docs.microsoft.com/ru-ru/windows/win32/winmsg/using-messages-and-message-queues]
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
    tab_buttons.clear(); // may be unnecessary
    current_tab.reset(); // may be unnecessary

    return msg.wParam;
}
