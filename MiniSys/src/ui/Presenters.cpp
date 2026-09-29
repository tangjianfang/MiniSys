#include "ui/Presenters.h"

#include "core/AppScanner.h"
#include "core/DeleteOp.h"
#include "core/FolderTreeScanner.h"
#include "core/JunkScanner.h"
#include "core/LargeFileScanner.h"
#include "core/OperationLog.h"
#include "core/PlanBuilder.h"
#include "core/SessionService.h"
#include "res/resource.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>

namespace minisys {

// =====================================================================
// ListTabPresenter
// =====================================================================

ListTabPresenter::ListTabPresenter(TabId tab, UiHandles ui)
    : TabPresenter(tab), ui_(ui) {}

void ListTabPresenter::OnActivate() {
    // v1 reset the shared sort state on every tab change.
    sortCol_ = -1;
    sortAsc_ = false;
}

void ListTabPresenter::Refresh() {
    RenderItems();
}

void ListTabPresenter::RenderItems() {
    ListView_DeleteAllItems(ui_.list);
    auto& items = SessionService::Instance().MutableResults(tab_);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        LVITEMW lvi{}; lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = static_cast<int>(i);
        lvi.pszText = const_cast<LPWSTR>(it.category.c_str());
        lvi.lParam  = static_cast<LPARAM>(i);
        int row = ListView_InsertItem(ui_.list, &lvi);
        ListView_SetItemText(ui_.list, row, 1, const_cast<LPWSTR>(it.title.c_str()));
        std::wstring sz = FormatSize(it.sizeBytes);
        ListView_SetItemText(ui_.list, row, 2, sz.data());
        ListView_SetItemText(ui_.list, row, 3, const_cast<LPWSTR>(it.detail.c_str()));
        if (it.recommended) ListView_SetCheckState(ui_.list, row, TRUE);
    }
}

std::vector<size_t> ListTabPresenter::CollectChecked() const {
    std::vector<size_t> out;
    int n = ListView_GetItemCount(ui_.list);
    for (int i = 0; i < n; ++i) {
        if (!ListView_GetCheckState(ui_.list, i)) continue;
        LVITEMW lvi{}; lvi.iItem = i; lvi.mask = LVIF_PARAM;
        ListView_GetItem(ui_.list, &lvi);
        out.push_back(static_cast<size_t>(lvi.lParam));
    }
    return out;
}

void ListTabPresenter::SortBySize() {
    if (sortCol_ == 0) sortAsc_ = !sortAsc_;
    else { sortCol_ = 0; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void ListTabPresenter::SortByTime() {
    if (sortCol_ == 1) sortAsc_ = !sortAsc_;
    else { sortCol_ = 1; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void ListTabPresenter::OnColumnClick(int col) {
    // col 2 = size, col 3 = detail; map col 2 -> sort size, others -> sort time
    int sortKey = (col == 2) ? 0 : 1;
    if (sortCol_ == sortKey) sortAsc_ = !sortAsc_;
    else { sortCol_ = sortKey; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void ListTabPresenter::ApplySortAndRefresh() {
    auto& items = SessionService::Instance().MutableResults(tab_);
    if (items.empty()) return;

    bool asc = sortAsc_;
    if (sortCol_ == 0) {
        std::stable_sort(items.begin(), items.end(),
            [asc](const ScanItem& a, const ScanItem& b) {
                return asc ? (a.sizeBytes < b.sizeBytes) : (a.sizeBytes > b.sizeBytes);
            });
    } else if (sortCol_ == 1) {
        std::stable_sort(items.begin(), items.end(),
            [asc](const ScanItem& a, const ScanItem& b) {
                return asc ? (a.createTime < b.createTime)
                           : (a.createTime > b.createTime);
            });
    }
    Refresh();
}

// =====================================================================
// Scan tab presenters
// =====================================================================

std::unique_ptr<Scanner> JunkPresenter::BuildScanner() {
    return std::make_unique<JunkScanner>();
}

std::unique_ptr<Scanner> LargeFilesPresenter::BuildScanner() {
    LargeFileScanner::Config cfg;

    // -- Min size (MB) --
    wchar_t szBuf[32] = {};
    GetWindowTextW(ui_.editMinSize, szBuf, 32);
    unsigned long long mb = _wtoi(szBuf);
    if (mb == 0) mb = 100;
    cfg.minBytes = mb * 1024ULL * 1024ULL;

    // -- Extension filter --
    wchar_t extBuf[512] = {};
    GetWindowTextW(ui_.editFileType, extBuf, 512);
    std::wstring extStr(extBuf);
    if (!extStr.empty()) {
        // Split by ";" and normalize each token
        std::wstring tok;
        for (auto ch : extStr) {
            if (ch == L';' || ch == L',') {
                if (!tok.empty()) {
                    if (tok[0] != L'.') tok = L'.' + tok;
                    cfg.extFilter.push_back(ToLower(tok));
                    tok.clear();
                }
            } else if (ch != L' ') {
                tok += ch;
            }
        }
        if (!tok.empty()) {
            if (tok[0] != L'.') tok = L'.' + tok;
            cfg.extFilter.push_back(ToLower(tok));
        }
    }

    // -- Drive filter --
    wchar_t drvBuf[128] = {};
    GetWindowTextW(ui_.editDrives, drvBuf, 128);
    std::wstring drvStr(drvBuf);
    if (!drvStr.empty()) {
        cfg.roots.clear();
        std::wstring token;
        for (auto ch : drvStr) {
            if (ch == L';' || ch == L',' || ch == L' ') {
                if (!token.empty()) {
                    // Accept "C" or "C:" or "C:\"
                    wchar_t letter = ::towupper(token[0]);
                    std::wstring root = {letter, L':', L'\\'};
                    cfg.roots.push_back(root);
                    token.clear();
                }
            } else {
                token += ch;
            }
        }
        if (!token.empty()) {
            wchar_t letter = ::towupper(token[0]);
            cfg.roots.push_back(std::wstring{letter, L':', L'\\'});
        }
    }

    return std::make_unique<LargeFileScanner>(std::move(cfg));
}

std::unique_ptr<Scanner> AppsPresenter::BuildScanner() {
    return std::make_unique<AppScanner>();
}

// =====================================================================
// FolderTreePresenter
// =====================================================================

FolderTreePresenter::FolderTreePresenter(UiHandles ui, SessionService& svc)
    : TabPresenter(TabId::FolderTree), ui_(ui), svc_(svc) {}

std::unique_ptr<Scanner> FolderTreePresenter::BuildScanner() {
    // Scan all fixed drives (the drive filter edit is not shown on this tab).
    return std::make_unique<FolderTreeScanner>();
}

void FolderTreePresenter::Refresh() {
    TreeView_DeleteAllItems(ui_.tree);
    itemPaths_.clear();

    auto& items = svc_.MutableResults(TabId::FolderTree);
    if (items.empty()) return;

    std::wstring lastDrive;
    HTREEITEM hDriveNode = nullptr;

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];

        // Insert drive node when drive changes
        if (it.category != lastDrive) {
            lastDrive = it.category;
            DiskSpace ds{};
            std::wstring driveLabel = it.category;
            if (QueryDiskSpace(it.category, ds)) {
                driveLabel = FormatW(L"%s  [可用 %s / 共 %s]",
                    it.category.c_str(),
                    FormatSize(ds.freeBytes).c_str(),
                    FormatSize(ds.totalBytes).c_str());
            }
            TVINSERTSTRUCTW tvis{};
            tvis.hParent      = TVI_ROOT;
            tvis.hInsertAfter = TVI_LAST;
            tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
            tvis.item.pszText = driveLabel.data();
            tvis.item.lParam  = -1;
            hDriveNode = TreeView_InsertItem(ui_.tree, &tvis);
        }

        // Insert folder node
        std::wstring label = FormatW(L"%s  (%s)", it.title.c_str(),
                                     FormatSize(it.sizeBytes).c_str());
        TVINSERTSTRUCTW tvis{};
        tvis.hParent      = hDriveNode;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
        tvis.item.pszText = label.data();
        tvis.item.lParam  = static_cast<LPARAM>(i);
        HTREEITEM hItem = TreeView_InsertItem(ui_.tree, &tvis);
        if (hItem) itemPaths_[hItem] = it.path;
    }

    // Expand all drive nodes
    HTREEITEM h = TreeView_GetRoot(ui_.tree);
    while (h) {
        TreeView_Expand(ui_.tree, h, TVE_EXPAND);
        h = TreeView_GetNextSibling(ui_.tree, h);
    }
}

bool FolderTreePresenter::OnContextMenu() {
    // Get the item under the cursor
    DWORD pos = GetMessagePos();
    POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
    POINT ptClient = pt;
    ScreenToClient(ui_.tree, &ptClient);

    TVHITTESTINFO hti{};
    hti.pt = ptClient;
    HTREEITEM hItem = TreeView_HitTest(ui_.tree, &hti);
    if (!hItem) return false;

    // Drive nodes have lParam == -1; only folder nodes are deletable
    auto it = itemPaths_.find(hItem);
    if (it == itemPaths_.end()) return false;

    auto folderPath = it->second;

    // Select the item
    TreeView_SelectItem(ui_.tree, hItem);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_DELETE, L"删除文件夹…");
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, ui_.main, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_DELETE) {
        std::wstring msg = FormatW(L"确定要删除文件夹:\n%s\n\n此操作将移入隔离区（可撤销）。",
            folderPath.wstring().c_str());
        if (MessageBoxW(ui_.main, msg.c_str(), L"确认删除",
                MB_OKCANCEL | MB_ICONWARNING) != IDOK) return false;

        auto& ftItems = svc_.MutableResults(TabId::FolderTree);
        size_t idx = static_cast<size_t>(-1);
        for (size_t i = 0; i < ftItems.size(); ++i) {
            if (ftItems[i].path == folderPath) { idx = i; break; }
        }
        if (idx == static_cast<size_t>(-1)) return false;

        // v2: single-item plan through the same GuardRails path; the tree is
        // rebuilt from the (mutated) results when WM_APP_OP_DONE arrives.
        auto plan = PlanBuilder::Build(TabId::FolderTree, ftItems, {idx});
        switch (svc_.ExecutePlan(plan)) {
            case SessionService::PlanStart::Started:
                return true;
            case SessionService::PlanStart::Busy:
                MessageBoxW(ui_.main, L"已有任务在进行中，请稍候。", L"提示",
                            MB_OK | MB_ICONINFORMATION);
                break;
            case SessionService::PlanStart::PlanStale:
                MessageBoxW(ui_.main, L"计划已过期，请重新扫描。", L"提示",
                            MB_OK | MB_ICONWARNING);
                break;
        }
    }
    return false;
}

// =====================================================================
// HistoryPresenter
// =====================================================================

HistoryPresenter::HistoryPresenter(UiHandles ui, SessionService& svc)
    : TabPresenter(TabId::History), ui_(ui), svc_(svc) {}

void HistoryPresenter::Refresh() {
    ListView_DeleteAllItems(ui_.list);
    auto recs = OperationLog::Instance().LoadAll();
    for (size_t i = 0; i < recs.size(); ++i) {
        const auto& r = recs[i];
        LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = static_cast<int>(i);
        std::wstring c0 = OperationLog::TypeToStr(r.type) + L" / " +
                          OperationLog::StatusToStr(r.status);
        it.pszText = c0.data();
        it.lParam = static_cast<LPARAM>(i);
        int row = ListView_InsertItem(ui_.list, &it);
        ListView_SetItemText(ui_.list, row, 1, const_cast<LPWSTR>(r.timestamp.c_str()));
        std::wstring sz = FormatSize(r.sizeBytes);
        ListView_SetItemText(ui_.list, row, 2, sz.data());
        std::wstring detail = r.source;
        if (!r.target.empty()) detail += L"  →  " + r.target;
        if (!r.note.empty())   detail += L"  | " + r.note;
        ListView_SetItemText(ui_.list, row, 3, const_cast<LPWSTR>(detail.c_str()));
    }
}

void HistoryPresenter::UndoSelected() {
    int sel = ListView_GetNextItem(ui_.list, -1, LVNI_SELECTED);
    if (sel < 0) {
        MessageBoxW(ui_.main, L"请选中一条历史记录。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto recs = OperationLog::Instance().LoadAll();
    if (sel >= static_cast<int>(recs.size())) return;
    const auto& r = recs[sel];

    std::wstring err;
    switch (svc_.UndoRecord(r, err)) {
        case SessionService::UndoResult::Ok:
            MessageBoxW(ui_.main, L"已撤销。", L"完成", MB_OK | MB_ICONINFORMATION);
            break;
        case SessionService::UndoResult::NotReversible:
            MessageBoxW(ui_.main, L"该记录不可撤销。", L"提示",
                        MB_OK | MB_ICONWARNING);
            break;
        case SessionService::UndoResult::DeleteType:
            MessageBoxW(ui_.main,
                L"回收站删除请手动从回收站还原（迁移与隔离区操作支持自动撤销）。",
                L"提示", MB_OK | MB_ICONINFORMATION);
            break;
        case SessionService::UndoResult::Failed:
            MessageBoxW(ui_.main, (L"撤销失败: " + err).c_str(), L"错误",
                        MB_OK | MB_ICONERROR);
            break;
    }
    Refresh();
}

} // namespace minisys
