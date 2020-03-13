#pragma once

#include "common.h"

extern HFONT button_font, button_font_bold, treeview_font;

class Button
{
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
    int width, height;
    const wchar_t *text;
    std::wstring stext;

    void paint(HDC target_dc, bool pressed);

public:
    UINT id;

    Button(UINT id, const wchar_t *text, int posx, int posy, int width_, int height_);
    Button(HWND dlg_wnd, UINT id);
    ~Button() {if (but_wnd) DestroyWindow(but_wnd);}

    void enable(bool e) {if (disabled != !e) disabled = !e, InvalidateRect(but_wnd, NULL, TRUE), EnableWindow(but_wnd, e);}
    void select(bool s) {if (selected !=  s) selected =  s, InvalidateRect(but_wnd, NULL, TRUE);}
};
