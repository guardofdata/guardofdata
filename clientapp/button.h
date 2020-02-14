#pragma once

#include "common.h"

extern HFONT button_font, button_font_bold;

class Button
{
    static WNDPROC btn_old_wnd_proc;
    static LRESULT CALLBACK button_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    enum class State
    {
        NORMAL,
        MOUSEOVER,
        PRESSED,
        SELECTED,
        DISABLED,
    };

    HWND but_wnd;
    bool hover = false, selected = false, disabled = false;
    int posx, posy, width, height;
    const wchar_t *text;

    void paint(HDC target_dc, bool pressed);

public:
    UINT id;

    Button(UINT id, const wchar_t *text, int posx_, int posy_, int width_, int height_) : id(id), text(text)
    {
        posx = mul_by_system_scaling_factor(posx_);
        posy = mul_by_system_scaling_factor(posy_);
        width = mul_by_system_scaling_factor(width_);
        height = mul_by_system_scaling_factor(height_);
        but_wnd = CreateWindow(L"BUTTON", L"", WS_VISIBLE|WS_CHILD|BS_OWNERDRAW, posx, posy, width, height, main_wnd, (HMENU)id, h_instance, NULL);
        WNDPROC lbtn_old_wnd_proc = (WNDPROC)SetWindowLongPtr(but_wnd, GWL_WNDPROC, (LONG_PTR)button_subclass_proc);
        if (btn_old_wnd_proc) ASSERT(btn_old_wnd_proc == lbtn_old_wnd_proc);
        btn_old_wnd_proc = lbtn_old_wnd_proc;
        SetWindowLongPtr(but_wnd, GWL_USERDATA, (LONG_PTR)this);
    }
    ~Button() {DestroyWindow(but_wnd);}

    void enable(bool e) {if (disabled != !e) disabled = !e, InvalidateRect(but_wnd, NULL, TRUE), EnableWindow(but_wnd, e);}
    void select(bool s) {if (selected !=  s) selected =  s, InvalidateRect(but_wnd, NULL, TRUE);}
};
