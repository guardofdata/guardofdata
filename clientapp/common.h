#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>


#undef ERROR
#define ERROR error_fn(__FILE__, __LINE__)
void error_fn(const char *file, int line);

#define ASSERT(expr) ((expr) || (assertion_failed(__FILE__, __LINE__), false))
void assertion_failed(const char *file, int line);

extern HINSTANCE h_instance;
extern HWND main_wnd, treeview_wnd, scrollbar_wnd;
extern HICON icon_dir_col, icon_dir_exp;

extern enum class BackupState
{
    SCAN_STARTED,
    SCAN_CANCELLED,
    SCAN_COMPLETED,
    BACKUP_STARTED,
} backup_state;

// [https://blog.softwareverify.com/how-to-make-your-mfc-or-non-mfc-program-support-high-dpi-monitors-the-easy-way/ <- https://www.codeproject.com/Messages/5452471/Re-create-a-dpi-aware-application.aspx <- google:‘codeproject dpiaware windows 7 site:www.codeproject.com’]
inline int mul_by_system_scaling_factor(int i)
{
    static int logpixelsx = 0;
    if (logpixelsx == 0) {
        HDC hdc = GetDC(NULL);
        logpixelsx = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
    }
    return i * logpixelsx / 96;
}
const int FONT_HEIGHT = mul_by_system_scaling_factor(16);
const int LINE_HEIGHT = FONT_HEIGHT + 2;
const int ICON_SIZE   = mul_by_system_scaling_factor(16);

// [http://web.archive.org/web/20100612190451/http://catch22.net/tuts/flicker <- http://web.archive.org/web/20100107165555/http://blogs.msdn.com/larryosterman/archive/2009/09/16/building-a-flicker-free-volume-control.aspx <- https://stackoverflow.com/questions/1842377/double-buffer-common-controls <- google:‘site:stackoverflow.com winapi tab switch flickering’]
class DoubleBufferedDC
{
    HDC target_dc, hdc;
    int width, height;
    HBITMAP hmembmp, holdbmp;

public:
    DoubleBufferedDC(HDC target_dc, int width, int height) : target_dc(target_dc), width(width), height(height)
    {
        hdc = CreateCompatibleDC(target_dc);
        hmembmp = CreateCompatibleBitmap(target_dc, width, height);
        holdbmp = SelectBitmap(hdc, hmembmp);
    }

    ~DoubleBufferedDC()
    {
        BitBlt(target_dc, 0, 0, width, height, hdc, 0, 0, SRCCOPY);
        SelectBitmap(hdc, holdbmp);
        DeleteBitmap(hmembmp);
        DeleteDC(hdc);
    }

    operator HDC() const {return hdc;}
};

inline void FillSolidRect(HDC hdc, const RECT &rect, COLORREF color)
{
    HBRUSH br = CreateSolidBrush(color);
    FillRect(hdc, &rect, br);
    DeleteObject(br);
}

class SelectPenAndBrush
{
    HDC hdc;
    HPEN pen, prev_pen;
    HBRUSH brush, prev_brush;

public:
    SelectPenAndBrush(HDC hdc, COLORREF pen_color, COLORREF brush_color, int pen_style = PS_SOLID, int pen_width = 1) : hdc(hdc)
    {
        pen = CreatePen(pen_style, pen_width, pen_color);
        prev_pen = SelectPen(hdc, pen);
        brush = CreateSolidBrush(brush_color);
        prev_brush = SelectBrush(hdc, brush);
    }

    ~SelectPenAndBrush()
    {
        SelectPen(hdc, prev_pen);
        DeletePen(pen);
        SelectBrush(hdc, prev_brush);
        DeleteBrush(brush);
    }
};

inline std::wstring operator/(const std::wstring &d, const std::wstring &f) {return d.back() == L'/' ? d + f : d + L'/' + f;}
inline std::wstring operator/(const std::wstring &d, const wchar_t      *f) {return d.back() == L'/' ? d + f : d + L'/' + f;}

inline std::wstring path_base_name(const std::wstring &path)
{
    size_t p = path.find_last_of(L"\\/");
    return p != std::wstring::npos ? path.substr(p + 1) : path;
}

inline void spin_lock_acquire(volatile long &lock) {if (_InterlockedExchange(&lock, 1)) while (lock || _InterlockedExchange(&lock, 1)) _mm_pause();}
inline void spin_lock_release(volatile long &lock) {_InterlockedExchange(&lock, 0);}

class SpinLock
{
    long lock = 0;
public:
    void acquire() {spin_lock_acquire(lock);}
    void release() {spin_lock_release(lock);}
};

inline void fast_make_lowercase_en(wchar_t *s)
{
    for (; *s; s++)
        if (unsigned(int(*s) - int(L'A')) <= unsigned(L'Z' - L'A'))
            *s += L'a' - L'A';
}

template <int N> inline bool ends_with(const std::wstring &s, const wchar_t (&suffix)[N])
{
    size_t suffix_len = N - 1;
    return s.length() >= suffix_len && memcmp(s.c_str() + s.length() - suffix_len, suffix, suffix_len*sizeof(wchar_t)) == 0;
}

inline std::wstring int_to_str(int i)
{
    wchar_t s[12];
    _itow_s(i, s, 10);
    return std::wstring(s);
}

inline std::wstring int64_to_str(int64_t i)
{
    wchar_t s[21];
    _i64tow_s(i, s, _countof(s), 10);
    return std::wstring(s);
}

template <typename Ty> inline std::string separate_thousands(Ty value)
{
    static std::stringstream ss;
    static bool initialized = false;
    if (!initialized) {
        struct Dotted : std::numpunct<char> // [https://stackoverflow.com/a/31656618/2692494 <- google:‘c++ separate thousands’]
        {
            char do_thousands_sep()   const {return ' ';}  // separate with spaces
            std::string do_grouping() const {return "\3";} // groups of 3 digits
        };
        ss.imbue(std::locale(ss.getloc(), new Dotted));
        ss << std::fixed << std::setprecision(1); // [https://stackoverflow.com/a/14432416/2692494 <- google:‘c++ cout format float digits’]
        initialized = true;
    }
    ss.str(std::string()); // [https://stackoverflow.com/a/20792/2692494 <- google:‘stringstream clear’] (`clear()` just clears state of the stream)
    ss << value;
    return ss.str();
}

inline std::wstring replace_all(const std::wstring &str, const std::wstring &old, const std::wstring &n)
{
    std::wstring s(str);
    size_t start_pos = 0;
    while ((start_pos = s.find(old, start_pos)) != s.npos) {
        s.replace(start_pos, old.length(), n);
        start_pos += n.length();
    }
    return s;
}
