#pragma once
#include <string>
#include <vector>
namespace RegionLens::setup
{
    struct TransactionFile { std::wstring staged, target, backup; bool saved{}, installed{}; };
    // Backend permits fault injection without changing Program Files in tests.
    template<class Backend> bool CommitFiles(std::vector<TransactionFile>& files, Backend& io)
    {
        auto restore = [&] {
            for (auto i = files.rbegin(); i != files.rend(); ++i)
            {
                if (i->installed && !io.Remove(i->target)) { io.RollbackFailed(); continue; }
                if (i->saved && !io.Move(i->backup, i->target)) io.RollbackFailed();
            }
        };
        for (auto& file : files)
        {
            if (io.Exists(file.backup)) { restore(); return false; }
            if (io.Exists(file.target))
            {
                if (!io.Move(file.target, file.backup)) { restore(); return false; }
                file.saved = true;
            }
            if (!io.Move(file.staged, file.target)) { restore(); return false; }
            file.installed = true;
        }
        for (auto& file : files) if (file.saved) io.Remove(file.backup);
        return true;
    }
}
