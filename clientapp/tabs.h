#pragma once

const int TAB_BUTTON_WIDTH  = 100;
const int TAB_BUTTON_HEIGHT = 40;

class Tab
{
public:
    virtual ~Tab() {}

    virtual int treeview_offsety() const {return 0;}
};

extern std::unique_ptr<Tab> current_tab;

#include "button.h"

class TabBackup : public Tab
{
    Button filter, cancel_scan;//, start_backup;

public:
    TabBackup() :
        filter(IDB_FILTER, L"Filter", 10, 60, 100, 30),
        cancel_scan(IDB_CANCEL_SCAN, L"Cancel scan. I want to configure all guarded folders manually", 120, 60, 400, 30)
    {}

    virtual int treeview_offsety() const override {return 40;}
};

class TabProgress : public Tab
{
};

class TabLog : public Tab
{
};

inline void switch_tab(std::unique_ptr<Tab> &&t)
{
    current_tab = std::move(t);
}
