#include "ui/Presenters.h"

#include "core/AppScanner.h"
#include "core/DeleteOp.h"
#include "core/FolderTreeScanner.h"
#include "core/JunkScanner.h"
#include "core/LargeFileScanner.h"
#include "core/OperationLog.h"
#include "core/PlanBuilder.h"
#include "core/SessionService.h"
#include "core/VolumeIndex.h"
#include "res/resource.h"
#include "ui/Dialogs.h"
#include "ui/Layout.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <set>

namespace minisys {

namespace {
// REVIEW-UI P0/P1: shared helpers defined below (after the presenter code
// that first uses them).
void SetListColumns(HWND list,
                    const std::vector<std::pair<const wchar_t*, int>>& cols);
std::wstring LocalizeCategory(const std::wstring& c);
// REVIEW-UI P2 (L-10): header sort arrows; col < 0 clears all of them.
void UpdateHeaderSortArrows(HWND list, int col, bool asc);
// REVIEW-UI P2 (L-11): list cells are single-line — fold "\n" into " · ".
std::wstring OneLine(const std::wstring& s);
// review-07 X-9 (v2.10): "MM-dd HH:mm" for the 修改时间 column.
std::wstring FormatTimeShort(uint64_t filetime);
} // namespace

// =====================================================================
// ListTabPresenter
// =====================================================================

ListTabPresenter::ListTabPresenter(TabId tab, UiHandles ui)
    : TabPresenter(tab), ui_(ui) {}

void ListTabPresenter::OnActivate() {
    // v1 reset the shared sort state on every tab change. review-07 X-7:
    // sorting is the USER's, not the page's — keep it across switches and
    // re-show the arrow (the shared header's format resets on tab switch).
    UpdateHeaderSortArrows(ui_.list, sortCol_, sortAsc_);
}

void ListTabPresenter::Refresh() {
    // REVIEW P1-1: pull a UI-private copy; render/sort/collect/plans all
    // work on it from here on. REVIEW-UI P0 (L-1): capture the user's FULL
    // check state BEFORE the snapshot is replaced — explicit unchecking of
    // a recommended item must survive re-renders too.
    CaptureCheckState();
    CaptureViewState();   // X-7
    snapshot_ = SessionService::Instance().Results(tab_);
    RenderItems();
}

// Map row → path → checked, while the ListView rows and snapshot_ are still
// consistent (call before sorting or replacing the snapshot).
void ListTabPresenter::CaptureCheckState() {
    // v2.15: no-op — the virtual list holds no per-row check state;
    // checkStateByPath_ is the single source of truth, updated directly by
    // the LVN_ITEMCHANGED state-flip path in the main window.
}

// review-07 X-7: same window as CaptureCheckState — the rows still match the
// snapshot, so paths read here are trustworthy.
void ListTabPresenter::CaptureViewState() {
    viewTopPath_.clear();
    viewFocusPath_.clear();
    viewSelPaths_.clear();
    int top = ListView_GetTopIndex(ui_.list);
    if (const ScanItem* it = ItemAtRow(top)) viewTopPath_ = it->path.wstring();
    int idx = -1;
    while ((idx = ListView_GetNextItem(ui_.list, idx, LVNI_SELECTED)) >= 0) {
        if (const ScanItem* it = ItemAtRow(idx)) {
            viewSelPaths_.push_back(it->path.wstring());
        }
    }
    if ((idx = ListView_GetNextItem(ui_.list, -1, LVNI_FOCUSED)) >= 0) {
        if (const ScanItem* it = ItemAtRow(idx)) viewFocusPath_ = it->path.wstring();
    }
}

void ListTabPresenter::RestoreViewState() {
    if (viewTopPath_.empty() && viewFocusPath_.empty() && viewSelPaths_.empty()) {
        return;
    }
    int n = ListView_GetItemCount(ui_.list);
    int topRow = -1, focusRow = -1;
    std::vector<int> selRows;
    for (int r = 0; r < n; ++r) {
        const ScanItem* it = ItemAtRow(r);
        if (!it) continue;
        const std::wstring p = it->path.wstring();
        if (topRow < 0 && p == viewTopPath_) topRow = r;
        if (focusRow < 0 && !viewFocusPath_.empty() && p == viewFocusPath_) focusRow = r;
        for (const auto& s : viewSelPaths_) {
            if (p == s) { selRows.push_back(r); break; }
        }
    }
    if (!viewSelPaths_.empty() && selRows.empty()) {
        // Nothing survived — keep at least a focus point.
        if (topRow < 0 && focusRow < 0 && n > 0) topRow = 0;
    }
    for (int r : selRows) {
        ListView_SetItemState(ui_.list, r, LVIS_SELECTED, LVIS_SELECTED);
    }
    if (focusRow >= 0) {
        ListView_SetItemState(ui_.list, focusRow, LVIS_FOCUSED, LVIS_FOCUSED);
    } else if (topRow >= 0) {
        ListView_SetItemState(ui_.list, topRow, LVIS_FOCUSED, LVIS_FOCUSED);
    }
    if (topRow >= 0) ListView_EnsureVisible(ui_.list, topRow, FALSE);
    else if (focusRow >= 0) ListView_EnsureVisible(ui_.list, focusRow, FALSE);
}

const ScanItem* ListTabPresenter::ItemAtRow(int row) const {
    // L-3 (round-5 07): virtual mode — rows map 1:1 onto snapshot_ (sorting
    // permutes the SNAPSHOT, not the rows), and LVIF_PARAM is meaningless
    // under LVS_OWNERDATA; the old lParam indirection could fetch the wrong
    // item for the double-click info panel and the row tints.
    if (row < 0 || static_cast<size_t>(row) >= snapshot_.size()) return nullptr;
    return &snapshot_[static_cast<size_t>(row)];
}

void ListTabPresenter::RenderItems() {
    // v2.15 (round-4 02 P0): VIRTUAL rendering — no per-row inserts. The
    // columns are still switched per tab; check state lives in
    // checkStateByPath_ (seeded from `recommended` for unseen paths);
    // text is supplied on demand via SupplyDispInfo.
    const std::vector<std::pair<const wchar_t*, int>> kScanCols = {
        { L"分类", 150 }, { L"风险", 100 }, { L"项目", 300 },
        { L"大小", 90 }, { L"详情", 260 }, { L"修改时间", 110 },
    };
    HWND hdr = ListView_GetHeader(ui_.list);
    int oldColCount = hdr ? Header_GetItemCount(hdr) : 0;
    std::vector<int> oldWidths;
    for (int i = 0; i < oldColCount; ++i) {
        oldWidths.push_back(ListView_GetColumnWidth(ui_.list, i));
    }
    SetListColumns(ui_.list, kScanCols);
    if (oldColCount == static_cast<int>(kScanCols.size())) {
        for (int i = 0; i < oldColCount; ++i) {
            if (oldWidths[i] > 0) ListView_SetColumnWidth(ui_.list, i, oldWidths[i]);
        }
    }
    SyncCheckDefaults();
    // Pre-render cell texts (allocation happens once here, not per paint).
    cells_.assign(snapshot_.size() * kScanCols.size(), std::wstring());
    for (size_t i = 0; i < snapshot_.size(); ++i) {
        const auto& it = snapshot_[i];
        std::wstring sz = it.sizeBytes ? FormatSize(it.sizeBytes) : std::wstring(L"—");
        cells_[i * 6 + 0] = LocalizeCategory(it.category);
        cells_[i * 6 + 1] = RiskBadge(it);
        cells_[i * 6 + 2] = it.title;
        cells_[i * 6 + 3] = std::move(sz);
        cells_[i * 6 + 4] = OneLine(it.detail);
        cells_[i * 6 + 5] = FormatTimeShort(it.createTime);
    }
    SendMessageW(ui_.list, LVM_SETITEMCOUNT,
                 static_cast<WPARAM>(snapshot_.size()), 0);
    RestoreViewState();   // X-7: top row / focus / selection by path
}

// v2.15: seed the authoritative check map for paths not seen yet.
void ListTabPresenter::SyncCheckDefaults() {
    for (const auto& it : snapshot_) {
        auto key = ToLower(it.path.wstring());
        checkStateByPath_.try_emplace(key, it.recommended);
    }
}

// v2.15: virtual cell supply.
// v2.16d CRASH FIX (WER 0xc000041d @ Presenters.cpp:191, source-line
// symbolized via dbghelp): cells_ can be stale/smaller than snapshot_
// across tab switches and background prunes — the row check alone allowed
// an out-of-range vector index inside the ListView callback, which took
// the whole process down mid-startup ("silent death" sessions). Every
// access is now bounds-checked; on mismatch we answer nothing (blank cell)
// instead of crashing.
bool ListTabPresenter::SupplyDispInfo(NMLVDISPINFOW* di) {
    int row = di->item.iItem;
    if (row < 0 || static_cast<size_t>(row) >= snapshot_.size()) return false;
    if (di->item.mask & LVIF_TEXT) {
        int col = di->item.iSubItem;
        if (col < 0 || col > 5) return false;
        size_t idx = static_cast<size_t>(row) * 6 + static_cast<size_t>(col);
        if (idx >= cells_.size()) {
            di->item.pszText[0] = L'\0';
            return true;
        }
        wcsncpy_s(di->item.pszText, di->item.cchTextMax, cells_[idx].c_str(),
                  _TRUNCATE);
    }
    if (di->item.mask & LVIF_STATE) {
        // Checkbox image state comes from the authoritative map.
        if (di->item.stateMask & LVIS_STATEIMAGEMASK) {
            auto key = ToLower(snapshot_[static_cast<size_t>(row)].path.wstring());
            auto it = checkStateByPath_.find(key);
            bool checked = it != checkStateByPath_.end() && it->second;
            di->item.state = INDEXTOSTATEIMAGEMASK(checked ? 2 : 1);
        }
    }
    return true;
}

void ListTabPresenter::OnCheckFlipped(int row, bool checked) {
    if (row < 0 || static_cast<size_t>(row) >= snapshot_.size()) return;
    checkStateByPath_[ToLower(snapshot_[row].path.wstring())] = checked;
}

bool ListTabPresenter::ToggleCheckAt(int row) {
    if (row < 0 || static_cast<size_t>(row) >= snapshot_.size()) return false;
    auto key = ToLower(snapshot_[row].path.wstring());
    auto it = checkStateByPath_.find(key);
    bool now = (it == checkStateByPath_.end()) ? !snapshot_[row].recommended
                                               : !it->second;
    checkStateByPath_[key] = now;
    ListView_RedrawItems(ui_.list, row, row);
    UpdateWindow(ui_.list);
    return now;
}

// v2.15: bulk check toggle without any per-row control messages.
void ListTabPresenter::SetAllChecked(bool on) {
    for (const auto& it : snapshot_) {
        checkStateByPath_[ToLower(it.path.wstring())] = on;
    }
    // State changed for every row — repaint.
    ListView_RedrawItems(ui_.list, 0, ListView_GetItemCount(ui_.list) - 1);
    UpdateWindow(ui_.list);
}

namespace {

// REVIEW-UI P0 (L-2): shared column header/width switcher (free function —
// used by both the scan-tab presenters and HistoryPresenter). v2.5: widths
// DPI-scaled (L-12); the format is reset so sort arrows from another tab's
// header never survive the switch.
// v2.13c visual-review fix: the 修改时间 column (v2.10) was never VISIBLE —
// this function only SET existing columns, but the ListView is created with
// five; a sixth must be INSERTED. Extra columns beyond the requested set
// (layout shrinking) are deleted.
void SetListColumns(HWND list,
                    const std::vector<std::pair<const wchar_t*, int>>& cols) {
    HWND hdr = ListView_GetHeader(list);
    int existing = hdr ? Header_GetItemCount(hdr) : 0;
    for (size_t i = 0; i < cols.size(); ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        // V-A1 (aesthetics): numeric columns right-align — sizes could not
        // be compared positionally when left-aligned.
        col.fmt = (wcscmp(cols[i].first, L"大小") == 0 ||
                   wcscmp(cols[i].first, L"修改时间") == 0)
                      ? LVCFMT_RIGHT
                      : LVCFMT_LEFT;
        col.pszText = const_cast<LPWSTR>(cols[i].first);
        col.cx = UiScale(list, cols[i].second);
        if (static_cast<int>(i) < existing) {
            SendMessageW(list, LVM_SETCOLUMNW, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&col));
        } else {
            SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&col));
        }
    }
    while (existing > static_cast<int>(cols.size())) {
        --existing;
        ListView_DeleteColumn(list, existing);
    }
}

// REVIEW-UI P2 (L-10): classic header arrows (HDF_SORTUP / HDF_SORTDOWN).
void UpdateHeaderSortArrows(HWND list, int col, bool asc) {
    HWND hd = ListView_GetHeader(list);
    if (!hd) return;
    int n = Header_GetItemCount(hd);
    for (int i = 0; i < n; ++i) {
        HDITEMW hdi{};
        hdi.mask = HDI_FORMAT;
        if (Header_GetItem(hd, i, &hdi)) {
            hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (i == col) hdi.fmt |= asc ? HDF_SORTUP : HDF_SORTDOWN;
            Header_SetItem(hd, i, &hdi);
        }
    }
}

// REVIEW-UI P2 (L-11): the detail text carries "\n" separators (path +
// hint); a list cell shows them as unprintable boxes — fold to one line.
std::wstring OneLine(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch == L'\n' || ch == L'\r') {
            if (!out.empty() && out.back() != L' ' &&
                out.back() != L'·') {
                out += L" · ";
            }
        } else {
            out += ch;
        }
    }
    return out;
}

// review-07 X-9: "MM-dd HH:mm" (local time) for the 修改时间 column.
std::wstring FormatTimeShort(uint64_t filetime) {
    if (filetime == 0) return L"—";
    ULARGE_INTEGER ul{};
    ul.QuadPart = filetime;
    FILETIME ft{};
    ft.dwLowDateTime  = ul.LowPart;
    ft.dwHighDateTime = ul.HighPart;
    FILETIME localFt{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &localFt) ||
        !FileTimeToSystemTime(&localFt, &st)) {
        return L"—";
    }
    wchar_t buf[32] = {};
    swprintf_s(buf, L"%02d-%02d %02d:%02d",
               st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// REVIEW-UI P1 (L-8): display-layer localization for the English category
// tokens stored in the rule table / scanners (storage unchanged).
std::wstring LocalizeCategory(const std::wstring& c) {
    static const std::map<std::wstring, std::wstring> kMap = {
        { L"System Temp", L"系统临时" },   { L"Explorer", L"资源管理器" },
        { L"Browser Cache", L"浏览器缓存" }, { L"Dev Cache", L"开发缓存" },
        { L"Windows Update", L"Windows 更新" }, { L"Windows Logs", L"系统日志" },
        { L"Chat Files", L"聊天文件" },   { L"System Component", L"系统组件" },
        { L"Dev Build", L"开发构建缓存" },
        { L"System Reserved", L"系统保留" }, { L"Recycle Bin", L"回收站" },
        { L"App", L"应用程序" },          { L"Image", L"图片" },
        { L"Video", L"视频" },            { L"Audio", L"音频" },
        { L"Archive", L"压缩包" },        { L"Installer", L"安装包" },
        { L"Document", L"文档" },         { L"VirtualDisk", L"虚拟磁盘" },
        { L"Other", L"其他" },
    };
    // "Duplicate (Video)" → "重复文件 · 视频"
    if (c.rfind(L"Duplicate (", 0) == 0 && c.size() > 13 && c.back() == L')') {
        auto inner = c.substr(11, c.size() - 12);
        return L"重复文件 · " + LocalizeCategory(inner);
    }
    auto it = kMap.find(c);
    return it != kMap.end() ? it->second : c;
}

} // namespace

// v2.2 (REVIEW P0-6): risk badge text for column 1. Rule-less items
// (large files / apps / folder tree) are honestly marked unclassified
// instead of guessed at.
// REVIEW-UI P2 (L-14): U+FE0F (emoji presentation selector) is appended to
// the pictographic marks — without it 🛡/⚠ can render as a monochrome box
// depending on the installed font set.
std::wstring ListTabPresenter::RiskBadge(const ScanItem& it) {
    // REVIEW-UI P1 (L-7): search results are unclassified by design — say so
    // instead of the meaningless "—".
    if (tab_ == TabId::Search) {
        return it.path == L"$RECYCLE.BIN" ? L"⚠️ 不可逆" : L"ℹ️ 未评估";
    }
    if (it.ruleId.empty() && it.path != L"$RECYCLE.BIN") return L"—";
    if (it.path == L"$RECYCLE.BIN") return L"⚠️ 不可逆";
    switch (it.riskLevel) {
        case RiskLevel::Safe:      return L"🛡️ 安全";
        case RiskLevel::Cautious:  return L"ℹ️ 谨慎";
        case RiskLevel::Advanced:  return L"⚠️ 系统组件";
        case RiskLevel::InfoOnly:  return L"⊘ 仅提示";
    }
    return L"—";
}

std::vector<size_t> ListTabPresenter::CollectChecked() const {
    // v2.15: the map is authoritative — one lookup per row, no control
    // messages (the old loop was 2 messages/row on every update).
    std::vector<size_t> out;
    out.reserve(snapshot_.size() / 4 + 1);
    for (size_t i = 0; i < snapshot_.size(); ++i) {
        auto it = checkStateByPath_.find(ToLower(snapshot_[i].path.wstring()));
        if (it != checkStateByPath_.end() && it->second) out.push_back(i);
    }
    return out;
}

// v2.7: "I deleted it by hand in Explorer — the list still shows it."
// v2.8: the existence checks run on the worker (VerifyPathsAsync); this
// applies the resulting dead-path list to the snapshot, preserving the
// user's explicit checks and syncing the service results (plan hash).
size_t ListTabPresenter::ApplyDeadPaths(const std::vector<std::wstring>& deadLower,
                                        bool render) {
    if (deadLower.empty() || snapshot_.empty()) return 0;
    CaptureCheckState();   // keep the user's explicit checks across the prune
    CaptureViewState();    // X-7

    std::vector<ScanItem> kept;
    kept.reserve(snapshot_.size());
    size_t removed = 0;
    for (auto& it : snapshot_) {
        // Recycle bin is a pseudo-item, not a path.
        if (it.path == L"$RECYCLE.BIN" ||
            std::find(deadLower.begin(), deadLower.end(),
                      ToLower(it.path.wstring())) == deadLower.end()) {
            kept.push_back(std::move(it));
        } else {
            ++removed;
        }
    }
    if (removed == 0) return 0;

    snapshot_.swap(kept);
    SessionService::Instance().StoreResults(tab_, snapshot_);
    if (render) RenderItems();   // review-04 R-1: only the active tab renders
    return removed;
}

void ListTabPresenter::SortBySize() {
    // Column 3 = 大小 (REVIEW-UI P2 L-10: sortCol_ is the column index now).
    if (sortCol_ == 3) sortAsc_ = !sortAsc_;
    else { sortCol_ = 3; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void ListTabPresenter::SortByTime() {
    // Column 5 = 修改时间 (X-9 gave time its own column).
    if (sortCol_ == 5) sortAsc_ = !sortAsc_;
    else { sortCol_ = 5; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void ListTabPresenter::OnColumnClick(int col) {
    // REVIEW-UI P2 (L-10): every column now sorts by what its header says
    // (分类 / 风险 / 项目 / 大小 / 详情 / 修改时间).
    if (sortCol_ == col) sortAsc_ = !sortAsc_;
    else { sortCol_ = col; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void ListTabPresenter::ApplySortAndRefresh() {
    // REVIEW-UI P0 (L-1): capture check state BEFORE the permutation — the
    // rows' lParams are only consistent with the pre-sort snapshot order.
    CaptureCheckState();
    CaptureViewState();   // X-7
    auto& items = snapshot_;   // UI-private (REVIEW P1-1)
    if (items.empty()) return;

    bool asc = sortAsc_;
    switch (sortCol_) {
        case 0:   // 分类 (then 项目 for stable groups)
            std::stable_sort(items.begin(), items.end(),
                [asc](const ScanItem& a, const ScanItem& b) {
                    int c = ToLower(a.category).compare(ToLower(b.category));
                    if (c != 0) return asc ? (c < 0) : (c > 0);
                    return asc ? (ToLower(a.title) < ToLower(b.title))
                               : (ToLower(a.title) > ToLower(b.title));
                });
            break;
        case 1:   // 风险 (severity order)
            std::stable_sort(items.begin(), items.end(),
                [asc](const ScanItem& a, const ScanItem& b) {
                    return asc ? (a.riskLevel < b.riskLevel)
                               : (a.riskLevel > b.riskLevel);
                });
            break;
        case 2:   // 项目 (case-insensitive)
            std::stable_sort(items.begin(), items.end(),
                [asc](const ScanItem& a, const ScanItem& b) {
                    return asc ? (ToLower(a.title) < ToLower(b.title))
                               : (ToLower(a.title) > ToLower(b.title));
                });
            break;
        case 3:   // 大小
            std::stable_sort(items.begin(), items.end(),
                [asc](const ScanItem& a, const ScanItem& b) {
                    return asc ? (a.sizeBytes < b.sizeBytes) : (a.sizeBytes > b.sizeBytes);
                });
            break;
        case 4:   // 详情 (text)
            std::stable_sort(items.begin(), items.end(),
                [asc](const ScanItem& a, const ScanItem& b) {
                    return asc ? (a.detail < b.detail) : (a.detail > b.detail);
                });
            break;
        case 5:   // 修改时间 (last write)
            std::stable_sort(items.begin(), items.end(),
                [asc](const ScanItem& a, const ScanItem& b) {
                    return asc ? (a.createTime < b.createTime)
                               : (a.createTime > b.createTime);
                });
            break;
        default:
            break;
    }
    RenderItems();
    UpdateHeaderSortArrows(ui_.list, sortCol_, sortAsc_);
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

// ---- SearchPresenter (v2.3) ----------------------------------------------

SearchPresenter::SearchPresenter(TabId tab, UiHandles ui)
    : ListTabPresenter(tab, ui) {}

std::unique_ptr<Scanner> SearchPresenter::BuildScanner() {
    return nullptr;   // the scan button is repurposed to rebuild the index
}

size_t SearchPresenter::TotalIndexed() const {
    // v2.11: total across ALL indexed volumes.
    return VolumeIndex::AnyValid() ? VolumeIndex::TotalEntries() : 0;
}

// REVIEW-UI P1 (04-1/U-1): the whole search pipeline now runs on the
// SessionService worker — SetQuery only records the request; the main
// window drives SearchAsync and WM_APP_SEARCH_DONE pulls the results.
void SearchPresenter::SetQuery(const std::wstring& text, bool matchPath) {
    query_ = text;
    matchPath_ = matchPath;
}

void SearchPresenter::Refresh() {
    // Results were stored by the worker under the service lock; pull a copy.
    // review-04 R-5: capture the user's explicit checks BEFORE replacing the
    // snapshot — same contract as the base class (L-1), otherwise an
    // explicitly unchecked search row reverts to `recommended` on the next
    // search-done refresh.
    CaptureCheckState();
    CaptureViewState();   // X-7
    snapshot_ = SessionService::Instance().Results(tab_);
    RenderItems();
}

// =====================================================================
// FolderTreePresenter
// =====================================================================

FolderTreePresenter::FolderTreePresenter(UiHandles ui, SessionService& svc)
    : TabPresenter(TabId::FolderTree), ui_(ui), svc_(svc) {}

std::unique_ptr<Scanner> FolderTreePresenter::BuildScanner() {
    // REVIEW P3 drill-down: with a focus root, scan its top level; a plain
    // 扫描 click clears the focus first (full-drive view).
    if (HasFocusRoot()) {
        FolderTreeScanner::Config cfg;
        cfg.drives = { focusRoot_.wstring() };
        ClearFocusRoot();
        return std::make_unique<FolderTreeScanner>(std::move(cfg));
    }
    return std::make_unique<FolderTreeScanner>();
}

void FolderTreePresenter::OnScanDone() {
    Refresh();
}

void FolderTreePresenter::Refresh() {
    TreeView_DeleteAllItems(ui_.tree);
    itemPaths_.clear();

    auto items = svc_.Results(TabId::FolderTree);   // copy (P1-1)
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

void ListTabPresenter::OnItemActivated(int row) {
    const ScanItem* it = ItemAtRow(row);
    if (it) ShowItemInfo(*it);
}

// REVIEW P1-6 (08-F3): the "why is this here / what happens / can I undo"
// panel. Runs off the rule table (ruleId → JunkRules) with an honest
// fallback for rule-less items (large files / apps / folder tree).
void ListTabPresenter::ShowItemInfo(const ScanItem& it) {
    std::wstring what = FormatW(L"%s\n%s", it.title.c_str(),
                                it.path.wstring().c_str());

    std::wstring consequence;
    std::wstring undoLine;
    if (it.ruleId.empty()) {
        consequence = L"非规则项（大文件/应用/文件夹），请自行判断是否需要。"
                      L"操作仍受安全闸保护（系统目录、云文件一律拒绝）。";
        undoLine = it.path == L"$RECYCLE.BIN"
                       ? L"清空回收站不可撤销。"
                       : L"移入隔离区，可在“操作历史”一键还原。";
    } else {
        switch (it.riskLevel) {
            case RiskLevel::Safe:
                consequence = L"缓存类内容，程序下次运行会自动重建。";
                break;
            case RiskLevel::Cautious:
                consequence = L"清理后系统或程序需要重新下载/重建，期间可能变慢。";
                break;
            case RiskLevel::Advanced:
                consequence = L"系统组件：仅通过官方系统命令处理，不会直接删除文件。";
                break;
            case RiskLevel::InfoOnly:
                consequence = L"系统关键文件，仅作展示。请按详情列的指引手动调整。";
                break;
        }
        if (it.strategy == CleanStrategy::Delegate) {
            consequence += L"\n将执行: " + it.command;
            undoLine = L"系统命令的效果不可自动撤销。";
        } else if (it.strategy == CleanStrategy::InfoOnly) {
            undoLine = L"不可执行。";
        } else {
            undoLine = L"移入隔离区，可在“操作历史”一键还原；清空隔离区后才真正释放空间。";
        }
    }

    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = ui_.main;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"项目说明";
    tc.pszMainIcon = TD_INFORMATION_ICON;
    // REVIEW-UI P1 (04-3): `it` references the presenter snapshot; a queued
    // search timer firing inside the TaskDialog modal pump could replace it.
    // Copy the strings the dialog will keep redrawing.
    std::wstring titleCopy = it.title;
    tc.pszMainInstruction = titleCopy.c_str();
    std::wstring content = L"【这是什么】\n" + what +
                           L"\n\n【处理后会怎样】\n" + consequence +
                           L"\n\n【能否还原】\n" + undoLine;
    tc.pszContent = content.c_str();
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    int pressed = 0;
    TaskDialogIndirect(&tc, &pressed, nullptr, nullptr);
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
    // REVIEW-UI P1 (L-8): the action is quarantine, not deletion — the verb
    // must match the confirmation dialog.
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_DELETE, L"移入隔离区…");
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, ui_.main, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_DELETE) {
        // REVIEW P0-6: destructive confirmation defaults to CANCEL; wording
        // matches the actual operation (07-X13: "删除" was misleading).
        // REVIEW-UI P2 (L-9): unified TaskDialog confirm (default CANCEL).
        if (!dialogs::ConfirmTask(ui_.main, L"移入隔离区",
                L"确定要将文件夹移入隔离区吗？",
                folderPath.wstring() + L"\n\n可在“操作历史”中一键还原。",
                /*warning=*/true, L"移入隔离区")) {
            return false;
        }

        auto ftItems = svc_.Results(TabId::FolderTree);   // copy (P1-1)
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
                // REVIEW P0-5 (03-B5): this path used to run without
                // disabling anything — tell the main window to lock the
                // action matrix while the plan executes.
                svc_.NotifyTaskStarted();   // R-3: gen-stamped (wp=0 was dropped)
                return true;
            case SessionService::PlanStart::Busy:
                SetWindowTextW(ui_.info, L"ℹ 已有任务在进行中，请稍候。");
                break;
            case SessionService::PlanStart::PlanStale:
                SetWindowTextW(ui_.info, L"⚠ 计划已过期，请重新扫描。");
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
    // v2.15: virtual mode — keep the records + pre-rendered cells, set the
    // item count; SupplyDispInfo serves the rest.
    SetListColumns(ui_.list, {
        { L"类型", 170 }, { L"时间", 150 }, { L"大小", 90 },
        { L"详情（来源 → 去向）", 420 }, { L"状态", 110 },
    });
    rows_ = OperationLog::Instance().LoadAll();
    cells_.assign(rows_.size() * 5, std::wstring());
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        std::wstring detail = r.source;
        if (!r.target.empty()) detail += L"  →  " + r.target;
        if (!r.note.empty())   detail += L"  | " + r.note;
        cells_[i * 5 + 0] = HistoryTypeLabel(r);
        cells_[i * 5 + 1] = r.timestamp;
        cells_[i * 5 + 2] = FormatSize(r.sizeBytes);
        cells_[i * 5 + 3] = OneLine(detail);
        cells_[i * 5 + 4] = HistoryRiskLabel(r);
    }
    SendMessageW(ui_.list, LVM_SETITEMCOUNT,
                 static_cast<WPARAM>(rows_.size()), 0);
}

bool HistoryPresenter::SupplyDispInfo(NMLVDISPINFOW* di) {
    int row = di->item.iItem;
    if (row < 0 || static_cast<size_t>(row) >= rows_.size()) return false;
    if (di->item.mask & LVIF_TEXT) {
        int col = di->item.iSubItem;
        if (col < 0 || col > 4) return false;
        size_t idx = static_cast<size_t>(row) * 5 + static_cast<size_t>(col);
        if (idx >= cells_.size()) {   // v2.16d crash guard (see above)
            di->item.pszText[0] = L'\0';
            return true;
        }
        wcsncpy_s(di->item.pszText, di->item.cchTextMax, cells_[idx].c_str(),
                  _TRUNCATE);
    }
    return true;   // no checkboxes on this page
}

std::wstring HistoryPresenter::HistoryTypeLabel(const OpRecord& r) {
    std::wstring type;
    switch (r.type) {
        case OpType::Quarantine:      type = L"已隔离"; break;
        case OpType::MoveAndJunction: type = L"已迁移"; break;
        case OpType::EmptyRecycleBin: type = L"清空回收站"; break;
        case OpType::Delegate:        type = L"系统命令"; break;
        case OpType::DeleteToRecycleBin: default: type = L"回收站删除"; break;
    }
    switch (r.status) {
        case OpStatus::Success:      return type + L" · 成功";
        case OpStatus::Reverted:     return type + L" · 已还原";
        case OpStatus::Failed:       return type + L" · 失败";
        case OpStatus::Interrupted:  return type + L" · 中断";
        case OpStatus::Pending: default: return type;
    }
}

std::wstring HistoryPresenter::HistoryRiskLabel(const OpRecord& r) {
    // X-27a (round-4 07): unsigned records still advertised "🛡 可还原" —
    // undo refuses them; the column must say so.
    if (!OperationLog::VerifySignature(r)) return L"未签名";
    if (r.status == OpStatus::Failed ||
        r.status == OpStatus::Interrupted) return L"需关注";
    if (r.type == OpType::EmptyRecycleBin) return L"不可逆";
    if (!r.isReversible) return L"不可逆";
    return L"可还原";
}

void HistoryPresenter::UndoSelected() {
    // REVIEW P1-4 (07-X9): multi-select batch undo, async on the worker —
    // one 18-item quarantine batch used to need 36+ clicks and each
    // migration undo froze the UI for the whole copy-back.
    std::vector<OpRecord> selected;
    int idx = -1;
    auto recs = OperationLog::Instance().LoadAll();
    while ((idx = ListView_GetNextItem(ui_.list, idx, LVNI_SELECTED)) >= 0) {
        if (idx < static_cast<int>(recs.size())) selected.push_back(recs[idx]);
    }
    if (selected.empty()) {
        SetWindowTextW(ui_.info, L"ℹ 请先选中一条或多条历史记录。");
        return;
    }

    // Pre-flight eligibility classification for a clear hint.
    size_t eligible = 0, notReversible = 0, deleteType = 0, unsignedN = 0;
    for (const auto& r : selected) {
        // v2.14 (T-A1): unsigned / tampered records are display-only.
        if (!OperationLog::VerifySignature(r)) { ++unsignedN; }
        else if (!r.isReversible || r.status != OpStatus::Success) ++notReversible;
        else if (r.type != OpType::MoveAndJunction && r.type != OpType::Quarantine) ++deleteType;
        else ++eligible;
    }
    if (eligible == 0) {
        SetWindowTextW(ui_.info, unsignedN
            ? L"ℹ 选中记录缺少有效签名（升级前的旧记录或被篡改），禁止还原——可在命令面板中“信任并签名旧记录”。"
            : (deleteType
            ? L"ℹ 选中项均为回收站删除（请手动从回收站还原）。"
            : L"ℹ 选中记录不可撤销（仅成功且可逆的记录支持）。"));
        return;
    }

    if (!svc_.UndoRecordsAsync(selected)) {
        SetWindowTextW(ui_.info, L"ℹ 已有任务在进行中，请稍候。");
        return;
    }
    svc_.NotifyTaskStarted();   // R-3: gen-stamped (wp=0 was dropped)
}

} // namespace minisys
