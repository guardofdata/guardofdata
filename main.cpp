#include "precompiled.h"
#include "resource.h"
#include "tabs.h"

const UINT WM_TRAY = WM_USER;
#define RECTARGS(rect) (rect).left, (rect).top, (rect).right-(rect).left, (rect).bottom-(rect).top

HINSTANCE h_instance;
HWND main_wnd, treeview_wnd;
std::vector<std::unique_ptr<Button>> tab_buttons;
std::unique_ptr<Tab> current_tab;

RECT calc_treeview_wnd_rect()
{
    RECT cr;
    GetClientRect(main_wnd, &cr);
    RECT r = {
        mul_by_system_scaling_factor(10),
        mul_by_system_scaling_factor(10 + TAB_BUTTON_HEIGHT + 10 + current_tab->treeview_offsety()),
        cr.right  - mul_by_system_scaling_factor(10),
        cr.bottom - mul_by_system_scaling_factor(10),
    };
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
            Shell_NotifyIcon(NIM_DELETE, &stData);
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

    case WM_SIZE:
        {
        RECT treeview_wnd_rect = calc_treeview_wnd_rect();
        MoveWindow(treeview_wnd, RECTARGS(treeview_wnd_rect), TRUE);
        InvalidateRect(treeview_wnd, NULL, TRUE);
        }
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

        {SelectPenAndBrush spb(hdc, RGB(223, 223, 223), RGB(255, 255, 255));
        Rectangle(hdc, 0, 0, r.right, r.bottom);}

        }
        EndPaint(hwnd, &ps);
        return 0;
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

    LOGFONT lf = {mul_by_system_scaling_factor(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH|FF_SWISS, _T("")};
    button_font = CreateFontIndirect(&lf);
    lf.lfWeight = FW_BOLD;
    button_font_bold = CreateFontIndirect(&lf);

    tab_buttons.push_back(std::make_unique<Button>(IDB_TAB_BACKUP,   L"Backup",   10,                      10, TAB_BUTTON_WIDTH + 1, TAB_BUTTON_HEIGHT));
    tab_buttons.push_back(std::make_unique<Button>(IDB_TAB_PROGRESS, L"Progress", 10 +   TAB_BUTTON_WIDTH, 10, TAB_BUTTON_WIDTH + 1, TAB_BUTTON_HEIGHT));
    tab_buttons.push_back(std::make_unique<Button>(IDB_TAB_LOG,      L"Log",      10 + 2*TAB_BUTTON_WIDTH, 10, TAB_BUTTON_WIDTH + 1, TAB_BUTTON_HEIGHT));
    SendMessage(main_wnd, WM_COMMAND, IDB_TAB_BACKUP, 0);

    {// Tree View child window is common between all tabs to avoid flickering during tab switching (when one Tree View is destroyed and another is created)
    WNDCLASS wc = {0};
    wc.lpfnWndProc = treeview_wnd_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"Guard of Data tree view window class";
    if (!RegisterClass(&wc)) ERROR;
    RECT treeview_wnd_rect = calc_treeview_wnd_rect();
    treeview_wnd = CreateWindowEx(0, wc.lpszClassName, L"", WS_CHILD|WS_VISIBLE, RECTARGS(treeview_wnd_rect), main_wnd, NULL, hInstance, 0);
    if (!treeview_wnd) ERROR;
    }

    if (wcsstr(GetCommandLine(), L" --show-window"))
        ShowWindow(main_wnd, SW_NORMAL);

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

    tab_buttons.clear(); // may be unnecessary
    current_tab.reset(); // may be unnecessary

    return msg.wParam;
}
