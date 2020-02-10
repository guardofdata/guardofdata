#include "precompiled.h"
#include "resource.h"

const UINT WM_TRAY = WM_USER;
#define RECTARGS(rect) (rect).left, (rect).top, (rect).right-(rect).left, (rect).bottom-(rect).top

#undef ERROR
#define ERROR do {} while(false)

HINSTANCE h_instance;
HWND main_wnd;

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
        ShowWindow(hwnd, SW_HIDE);
        return 0; // to not process message by the system, which call DestroyWindow in DefWindowProc

    case WM_TRAY:
        switch (lparam)
        {
        case WM_LBUTTONDBLCLK:
            ShowWindow(hwnd, SW_SHOW);
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
                    ShowWindow(hwnd, SW_SHOW);
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
    WNDCLASS wc = {0};
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLIENTAPP));
    //wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
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

    if (wcsstr(GetCommandLine(), L" --show-window"))
        ShowWindow(main_wnd, SW_NORMAL);

    MSG msg;
    BOOL ret;
    // [https://docs.microsoft.com/ru-ru/windows/win32/winmsg/using-messages-and-message-queues]
    while((ret = GetMessage(&msg, NULL, 0, 0 )) != 0) {
        if (ret == -1) {
            ERROR;
        }
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return msg.wParam;
}
