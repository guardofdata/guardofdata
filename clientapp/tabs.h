﻿#pragma once
#include "common.h"

const int TAB_BUTTON_WIDTH  = 100;
const int TAB_BUTTON_HEIGHT = 40;

const int TREEVIEW_PADDING = mul_by_system_scaling_factor(5);

class Tab
{
public:
    virtual ~Tab() {}

    virtual int treeview_offsety() const {return 0;}
    virtual void treeview_paint(HDC hdc, int width, int height) = 0;
};

extern std::unique_ptr<Tab> current_tab;

#include "button.h"
#include "resource.h"

class TabBackup : public Tab
{
    Button filter, cancel_scan;//, start_backup;

public:
    TabBackup() :
        filter(IDB_FILTER, L"Filter", 10, 60, 100, 30),
        cancel_scan(IDB_CANCEL_SCAN, L"Cancel scan. I want to configure all guarded folders manually", 120, 60, 400, 30)
    {}

    virtual int treeview_offsety() const override {return 40;}
    virtual void treeview_paint(HDC hdc, int width, int height) override;
};

class TabProgress : public Tab
{
    virtual void treeview_paint(HDC hdc, int width, int height) override {}
};

class TabLog : public Tab
{
    virtual void treeview_paint(HDC hdc, int width, int height) override {}
};

inline void switch_tab(std::unique_ptr<Tab> &&t)
{
    current_tab = std::move(t);
}
