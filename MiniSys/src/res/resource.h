#pragma once

#define IDI_APPICON                 101

// Tab control + child windows
#define IDC_TABCTRL                 1001
#define IDC_STATUSBAR               1002
#define IDC_PROGRESSBAR             1003

// Common per-tab control IDs (re-used in each tab page)
#define IDC_LISTVIEW                2001
#define IDC_BTN_SCAN                2002
#define IDC_BTN_EXECUTE             2003
#define IDC_BTN_UNDO                2004
#define IDC_BTN_OPENLOC             2005
#define IDC_BTN_REFRESH             2006
#define IDC_BTN_CHOOSE_TARGET       2007
#define IDC_CHK_ADVANCED            2008
#define IDC_LABEL_INFO              2009
#define IDC_BTN_DEDUP               2010

// LargeFiles tab: settings panel controls
#define IDC_EDIT_MINSIZE            2011  // min file size (MB)
#define IDC_STATIC_MINSIZE_UNIT     2012  // "MB" label
#define IDC_EDIT_FILETYPE           2013  // extension filter ".mp4;.mkv"
#define IDC_EDIT_DRIVES             2014  // drive letters "C;D"
#define IDC_TREEVIEW                2015  // folder-tree TreeView

// Sort buttons (LargeFiles / all scan tabs)
#define IDC_BTN_SORT_SIZE           2016
#define IDC_BTN_SORT_TIME           2017

// History tab
#define IDC_BTN_EMPTY_Q             2018  // 清空隔离区
#define IDC_BTN_ABOUT               2019  // 关于

// Accelerators (keyboard navigation, REVIEW P2)
#define IDC_ACCEL_SELECTALL         2090  // Ctrl+A — toggle all rows
// v2.3: instant search tab
#define IDC_EDIT_SEARCH             2022
#define IDC_CHK_MATCHPATH           2023
#define IDC_BTN_QUICKFILTER         2025  // v2.6: quick-filter preset menu
#define TIMER_SEARCH_DEBOUNCE       2
#define TIMER_SEARCH_RETRY          3     // retry while a previous search drains
#define TIMER_IDLE_REFRESH          4     // v2.5: idle-time background rescan
#define TIMER_VERIFY_LIST           5     // v2.7: debounced focus-return verify
#define TIMER_IDLE_COUNTDOWN        6     // v2.10: idle rescan grace countdown
#define TIMER_TOUR                  7     // v2.13c: -tour visual-review walk
#define IDC_ACCEL_PALETTE           2091  // v2.10: Ctrl+K command palette
// v2.4 (REVIEW-UI P1): async search completion
#define WM_APP_SEARCH_DONE          (WM_APP + 6)
// v2.8: worker-side list verify / GuardRails preview completion
#define WM_APP_VERIFY_DONE          (WM_APP + 7)
#define WM_APP_PREVIEW_DONE         (WM_APP + 8)
// Context menu on the list (REVIEW P2)
#define IDM_LIST_OPEN               3002
#define IDM_LIST_INFO               3003
#define IDM_LIST_SELECTALL          3004
#define IDM_LIST_SELECTNONE         3005
#define IDM_LIST_PREVIEW            3006
#define IDM_LIST_EXCLUDE            3007

// Custom messages
#define WM_APP_SCAN_PROGRESS        (WM_APP + 1)
#define WM_APP_SCAN_DONE            (WM_APP + 2)
#define WM_APP_OP_DONE              (WM_APP + 3)
#define WM_APP_OP_PROGRESS          (WM_APP + 4)
#define WM_APP_TASK_STARTED         (WM_APP + 5)  // any ExecutePlan start

// Context menu
#define IDM_CTX_DELETE              3001

// v2.6: search quick-filter presets (popup menu on the search row)
#define IDM_QF_PDB          3010   // ext:pdb
#define IDM_QF_OBJ          3011   // ext:obj
#define IDM_QF_BUILD_TMP    3012   // ext:ilk;idb;tlog;lastbuildstate
#define IDM_QF_PCH          3013   // ext:ipch;pch
#define IDM_QF_DIR_DEBUG    3014   // folder:debug
#define IDM_QF_DIR_RELEASE  3015   // folder:release
#define IDM_QF_DIR_BIN      3016   // folder:bin
#define IDM_QF_DIR_OBJ      3017   // folder:obj
#define IDM_QF_EXE          3018   // ext:exe;msi
#define IDM_QF_ARCHIVE      3019   // ext:zip;rar;7z
#define IDM_QF_VIDEO        3020   // ext:mp4;mkv;avi
#define IDM_QF_IMAGE        3021   // ext:jpg;jpeg;png;bmp
#define IDM_QF_AUDIO        3022   // ext:mp3;flac;wav
#define IDM_QF_DOC          3023   // ext:doc;docx;xls;xlsx;pdf
#define IDM_QF_CLEAR        3024   // strip all filter tokens
#define IDM_LIST_VERIFY     3025   // v2.7: verify rows, drop hand-deleted
