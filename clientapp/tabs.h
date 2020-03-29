#pragma once
#include "common.h"

const int TAB_BUTTON_WIDTH  = 100;
const int TAB_BUTTON_HEIGHT = 40;

const int TREEVIEW_PADDING = mul_by_system_scaling_factor(5);
const int LINE_PADDING_TOP = mul_by_system_scaling_factor(1);
const int LINE_PADDING_LEFT  = mul_by_system_scaling_factor(2);
const int LINE_PADDING_RIGHT = mul_by_system_scaling_factor(2);

class Tab
{
public:
    virtual ~Tab() {}

    virtual int treeview_offsety() const {return 0;}
    virtual void treeview_paint(HDC hdc, int width, int height) = 0;
    virtual void treeview_lbdown() = 0;
    virtual void treeview_rbdown() = 0;
};

extern std::unique_ptr<Tab> current_tab;

#include "button.h"
#include "resource.h"

class TabBackup : public Tab
{
    Button filter;
    std::unique_ptr<Button> cancel_scan_button, restart_scan, start_backup;

public:
    static enum class SortBy {NAME, SIZE, COUNT} sort_by;
    static volatile bool stop_scan, cancel_scan;

    TabBackup() :
        filter(IDB_FILTER, L"Filter", 10, 60, 100, 30)
    {
        if (backup_state == BackupState::SCAN_STARTED)
            cancel_scan_button = std::make_unique<Button>(IDB_CANCEL_SCAN, L"Cancel scan. I want to configure all guarded folders manually", 120, 60, 400, 30);
        else if (backup_state == BackupState::SCAN_COMPLETED || backup_state == BackupState::SCAN_CANCELLED)
            start_backup = std::make_unique<Button>(IDB_START_BACKUP, L"Start backup!", 120, 60, 100, 30);
        if (backup_state == BackupState::SCAN_CANCELLED)
            restart_scan = std::make_unique<Button>(IDB_RESTART_SCAN, L"Restart scan", 230, 60, 100, 30);
    }

    virtual int treeview_offsety() const override {return 40;}
    virtual void treeview_paint(HDC hdc, int width, int height) override;
    virtual void treeview_lbdown() override;
    virtual void treeview_rbdown() override;
};
INT_PTR CALLBACK backup_drive_selection_dlg_proc(HWND dlg_wnd, UINT message, WPARAM wparam, LPARAM lparam);

class TabProgress : public Tab
{
    //Button tree_view, flat_view, group_by_priority;
    virtual void treeview_paint(HDC hdc, int width, int height) override {}
    virtual void treeview_lbdown() override {}
    virtual void treeview_rbdown() override {}
};

class TabLog : public Tab
{
    virtual void treeview_paint(HDC hdc, int width, int height) override {}
    virtual void treeview_lbdown() override {}
    virtual void treeview_rbdown() override {}
};

inline void switch_tab(std::unique_ptr<Tab> &&t)
{
    current_tab = std::move(t);
}
