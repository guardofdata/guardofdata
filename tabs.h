#pragma once

class Tab
{
public:
    virtual ~Tab() {}
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
