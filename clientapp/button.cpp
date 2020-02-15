#include "precompiled.h"
#include "resource.h"
#include "button.h"


HFONT button_font, button_font_bold;
WNDPROC Button::btn_old_wnd_proc = NULL;

LRESULT CALLBACK Button::button_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    Button *btn = (Button*)GetWindowLongPtr(hwnd, GWL_USERDATA);

    switch (msg)
    {
    case WM_ERASEBKGND:
        return TRUE;

    case WM_PAINT:
        PAINTSTRUCT ps;
        btn->paint(BeginPaint(hwnd, &ps), (SendMessage(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0);
        EndPaint(hwnd, &ps);
        return 0;

    case WM_MOUSELEAVE:
        btn->hover = false;
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

//  case WM_SETCURSOR:
//      SetCursor(LoadCursor(NULL, IDC_HAND));
//      return TRUE;

    case WM_MOUSEMOVE:
        if (!btn->hover)
        {
            btn->hover = true;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd};
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;
    }

    return CallWindowProc(btn_old_wnd_proc, hwnd, msg, wparam, lparam);
}

void Button::paint(HDC target_dc, bool pressed)
{
    State state = disabled ? State::DISABLED : selected ? State::SELECTED : pressed ? State::PRESSED : (hover && GetAsyncKeyState(VK_LBUTTON) >= 0) ? State::MOUSEOVER : State::NORMAL;
    COLORREF text_color, bg_color;
    switch (state)
    {
    case State::SELECTED:
    case State::PRESSED:
    case State::MOUSEOVER:
        text_color = RGB(255, 255, 255);
        bg_color = RGB(0, 128, 0);
        break;
    case State::NORMAL:
        text_color = RGB(0, 0, 0);
        bg_color = RGB(255, 255, 255);
        break;
    }

    DoubleBufferedDC hdc(target_dc, width, height);

    RECT r;
    r.left = 0;
    r.right = width;
    r.top = 0;
    r.bottom = height;

    //FillSolidRect(hdc, r, bg_color);
    {SelectPenAndBrush spb(hdc, RGB(223, 223, 223), bg_color); // color 223 is taken from [https://www.pcmag.com/reviews/spideroak-one]
    Rectangle(hdc, 0, 0, width, height);}

    if (state == State::PRESSED) {
        r.left   += mul_by_system_scaling_factor(1);
        r.right  += mul_by_system_scaling_factor(1);
        r.top    += mul_by_system_scaling_factor(1);
        r.bottom += mul_by_system_scaling_factor(1);
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text_color);
    SelectFont(hdc, id <= IDB_TAB_LOG ? button_font_bold : button_font);
    DrawText(hdc, text, -1, &r, DT_CENTER|DT_SINGLELINE|DT_VCENTER);
}
