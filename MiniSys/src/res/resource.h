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

// Custom messages
#define WM_APP_SCAN_PROGRESS        (WM_APP + 1)
#define WM_APP_SCAN_DONE            (WM_APP + 2)
#define WM_APP_OP_DONE              (WM_APP + 3)

// Context menu
#define IDM_CTX_DELETE              3001
