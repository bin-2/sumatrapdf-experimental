/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/BitManip.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "wingui/LabelWithCloseWnd.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SumatraConfig.h"
#include "GlobalPrefs.h"
#include "Annotation.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DisplayModel.h"
#include "Favorites.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "AppTools.h"
#include "TableOfContents.h"
#include "Translations.h"
#include "Tabs.h"
#include "Toolbar.h"
#include "Selection.h"
#include "Menu.h"
#include "Accelerators.h"
#include "Theme.h"

#include "utils/Log.h"
#include "PdfBookmarkEditor.h"

/* Define if you want page numbers to be displayed in the ToC sidebar */
// #define DISPLAY_TOC_PAGE_NUMBERS

#ifdef DISPLAY_TOC_PAGE_NUMBERS
#define WM_APP_REPAINT_TOC (WM_APP + 1)
#endif

static void LayoutTocContainer(MainWindow* win);
static bool HasTocFilter(MainWindow* win);

// set tooltip for this item but only if the text isn't fully shown
// TODO: I might have lost something in translation
static void TocCustomizeTooltip(TreeView::GetTooltipEvent* ev) {
    auto treeView = ev->treeView;
    auto tm = treeView->treeModel;
    auto ti = ev->treeItem;
    auto nm = ev->info;
    TocItem* tocItem = (TocItem*)ti;
    IPageDestination* link = tocItem->GetPageDestination();
    if (!link) {
        return;
    }
    char* path = PageDestGetValue(link);
    if (!path) {
        path = tocItem->title;
    }
    if (!path) {
        return;
    }
    auto k = link->GetKind();
    // TODO: TocItem from Chm contain other types
    // we probably shouldn't set TocItem::dest there
    if (k == kindDestinationScrollTo) {
        return;
    }
    if (k == kindDestinationNone) {
        return;
    }

    bool isOk = (k == kindDestinationLaunchURL) || (k == kindDestinationLaunchFile) ||
                (k == kindDestinationLaunchEmbedded) || (k == kindDestinationMupdf) || (k = kindDestinationDjVu) ||
                (k == kindDestinationAttachment);
    ReportIf(!isOk);

    StrBuilder infotip;

    // Display the item's full label, if it's overlong
    RECT rcLine, rcLabel;
    treeView->GetItemRect(ev->treeItem, false, rcLine);
    treeView->GetItemRect(ev->treeItem, true, rcLabel);

    // TODO: this causes a duplicate. Not sure what changed
    if (false && rcLine.right + 2 < rcLabel.right) {
        char* currInfoTip = tm->Text(ti);
        infotip.Append(currInfoTip);
        infotip.Append("\r\n");
    }

    if (kindDestinationLaunchEmbedded == k || kindDestinationAttachment == k) {
        TempStr tmp = str::FormatTemp(_TRA("Attachment: %s"), path);
        infotip.Append(tmp);
    } else {
        infotip.Append(path);
    }

    str::BufSet(nm->pszText, nm->cchTextMax, infotip.Get());
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void RelayoutTocItem(LPNMTVCUSTOMDRAW ntvcd) {
    // code inspired by http://www.codeguru.com/cpp/controls/treeview/multiview/article.php/c3985/
    LPNMCUSTOMDRAW ncd = &ntvcd->nmcd;
    HWND hTV = ncd->hdr.hwndFrom;
    HTREEITEM hItem = (HTREEITEM)ncd->dwItemSpec;
    RECT rcItem;
    if (0 == ncd->rc.right - ncd->rc.left || 0 == ncd->rc.bottom - ncd->rc.top) return;
    if (!TreeView_GetItemRect(hTV, hItem, &rcItem, TRUE)) return;
    if (rcItem.right > ncd->rc.right) rcItem.right = ncd->rc.right;

    // Clear the label
    RECT rcFullWidth = rcItem;
    rcFullWidth.right = ncd->rc.right;
    FillRect(ncd->hdc, &rcFullWidth, GetSysColorBrush(COLOR_WINDOW));

    // Get the label's text
    WCHAR szText[MAX_PATH];
    TVITEM item;
    item.hItem = hItem;
    item.mask = TVIF_TEXT | TVIF_PARAM;
    item.pszText = szText;
    item.cchTextMax = MAX_PATH;
    TreeView_GetItem(hTV, &item);

    // Draw the page number right-aligned (if there is one)
    MainWindow* win = FindMainWindowByHwnd(hTV);
    TocItem* tocItem = (TocItem*)item.lParam;
    TempStr label = nullptr;
    if (tocItem->pageNo && win && win->IsDocLoaded()) {
        label = win->ctrl->GetPageLabeTemp(tocItem->pageNo);
        label = str::JoinTemp("  ", label);
    }
    if (label && str::EndsWith(item.pszText, label)) {
        RECT rcPageNo = rcFullWidth;
        InflateRect(&rcPageNo, -2, -1);

        SIZE txtSize;
        GetTextExtentPoint32(ncd->hdc, label, str::Len(label), &txtSize);
        rcPageNo.left = rcPageNo.right - txtSize.cx;

        SetTextColor(ncd->hdc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(ncd->hdc, GetSysColor(COLOR_WINDOW));
        DrawTextW(ncd->hdc, label, -1, &rcPageNo, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        // Reduce the size of the label and cut off the page number
        rcItem.right = std::max(rcItem.right - txtSize.cx, 0);
        szText[str::Len(szText) - str::Len(label)] = '\0';
    }

    SetTextColor(ncd->hdc, ntvcd->clrText);
    SetBkColor(ncd->hdc, ntvcd->clrTextBk);

    // Draw the focus rectangle (including proper background color)
    HBRUSH brushBg = CreateSolidBrush(ntvcd->clrTextBk);
    FillRect(ncd->hdc, &rcItem, brushBg);
    DeleteObject(brushBg);
    if ((ncd->uItemState & CDIS_FOCUS)) DrawFocusRect(ncd->hdc, &rcItem);

    InflateRect(&rcItem, -2, -1);
    DrawTextW(ncd->hdc, szText, -1, &rcItem, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_WORD_ELLIPSIS);
}
#endif

struct GoToTocLinkData {
    TocItem* tocItem;
    WindowTab* tab;
    DocController* ctrl;
};

enum class TocBookmarkDropMode {
    None,
    Before,
    After,
    AsChild,
    RootEnd,
};

struct TocBookmarkDragState {
    bool active = false;
    TocItem* source = nullptr;
    TocItem* target = nullptr;
    TocBookmarkDropMode mode = TocBookmarkDropMode::None;
    HTREEITEM sourceHItem = nullptr;
    HTREEITEM targetHItem = nullptr;
    HIMAGELIST dragImage = nullptr;
};

static TocBookmarkDragState gTocBookmarkDrag;


static void GoToTocLink(GoToTocLinkData* d) {
    AutoDelete delData(d);

    auto tab = d->tab;
    auto tocItem = d->tocItem;
    auto ctrl = d->ctrl;

    // validate tab before dereferencing — it may have been freed
    // while this task was queued (e.g. user closed the tab/window)
    if (!IsWindowTabValid(tab)) {
        return;
    }
    MainWindow* win = tab->win;
    // tocItem is invalid if the DocController has been replaced
    if (!IsMainWindowValid(win) || win->CurrentTab() != tab || tab->ctrl != ctrl) {
        return;
    }

    // make sure that the tree item that the user selected
    // isn't unselected in UpdateTocSelection right again
    win->tocKeepSelection = true;
    int pageNo = tocItem->pageNo;
    IPageDestination* dest = tocItem->GetPageDestination();
    if (dest) {
        ctrl->HandleLink(dest, win->linkHandler);
    } else if (pageNo) {
        ctrl->GoToPage(pageNo, true);
    }
    win->tocKeepSelection = false;
}

static bool IsScrollToLink(IPageDestination* link) {
    if (!link) {
        return false;
    }
    auto kind = link->GetKind();
    return kind == kindDestinationScrollTo;
}

static void GoToTocTreeItem(MainWindow* win, TreeItem ti, bool allowExternal) {
    if (!ti) {
        return;
    }
    TocItem* tocItem = (TocItem*)ti;
    bool validPage = (tocItem->pageNo > 0);
    bool isScroll = IsScrollToLink(tocItem->GetPageDestination());
    if (validPage || (allowExternal || isScroll)) {
        // delay changing the page until the tree messages have been handled
        auto data = new GoToTocLinkData;
        data->ctrl = win->ctrl;
        data->tocItem = tocItem;
        data->tab = win->CurrentTab();
        auto fn = MkFunc0<GoToTocLinkData>(GoToTocLink, data);
        uitask::Post(fn, "TaskGoToTocTreeItem");
    }
}

void ClearTocBox(MainWindow* win) {
    if (!win->tocLoaded) {
        return;
    }

    // set tocLoaded to false before SetText("") because SetText triggers
    // EN_CHANGE synchronously which calls ApplyTocFilter() re-entrantly
    // and we need it to bail out early
    win->tocLoaded = false;

    win->tocTreeView->Clear();

    // clear filter state
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    win->currPageNo = 0;
}

void ToggleTocBox(MainWindow* win) {
    if (!win->IsDocLoaded()) {
        return;
    }
    if (win->tocVisible) {
        SetSidebarVisibility(win, false, gGlobalPrefs->showFavorites);
        return;
    }
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    if (win->tocVisible) {
        HwndSetFocus(win->tocTreeView->hwnd);
    }
}

struct VistorForPageNoData {
    int pageNo = -1;

    TocItem* bestMatch = nullptr;
    int bestMatchPageNo = 0;
    int nItems = 0;
};

void visitTree(VistorForPageNoData* d, TreeItemVisitorData* vd) {
    auto tocItem = (TocItem*)vd->item;
    if (!tocItem) {
        return;
    }
    if (!d->bestMatch) {
        // if nothing else matches, match the root node
        d->bestMatch = tocItem;
    }
    ++d->nItems;
    int page = tocItem->pageNo;
    if ((page <= d->pageNo) && (page >= d->bestMatchPageNo) && (page >= 1)) {
        d->bestMatch = tocItem;
        d->bestMatchPageNo = page;
        if (d->pageNo == d->bestMatchPageNo) {
            // we can stop earlier if we found the exact match
            vd->stopTraversal = true;
            return;
        }
    }
}

// find the closest item in tree view to a given page number
static TocItem* TreeItemForPageNo(TreeView* treeView, int pageNo) {
    TreeModel* tm = treeView->treeModel;
    if (!tm) {
        return 0;
    }
    VistorForPageNoData d;
    d.pageNo = pageNo;
    auto fn = MkFunc1<VistorForPageNoData, TreeItemVisitorData*>(visitTree, &d);
    VisitTreeModelItems(tm, fn);
    // if there's only one item, we want to unselect it so that it can
    // be selected by the user
    if (d.nItems < 2) {
        return 0;
    }
    return d.bestMatch;
}

// TODO: I can't use TreeItem->IsExpanded() because it's not in sync with
// the changes user makes to TreeCtrl
static TocItem* FindVisibleParentTreeItem(TreeView* treeView, TocItem* ti) {
    if (!ti) {
        return nullptr;
    }
    while (true) {
        auto parent = ti->parent;
        if (parent == nullptr) {
            // ti is a root node
            return ti;
        }
        if (treeView->IsExpanded((TreeItem)parent)) {
            return ti;
        }
        ti = parent;
    }
    return nullptr;
}

void UpdateTocSelection(MainWindow* win, int currPageNo) {
    if (!win->tocLoaded || !win->tocVisible || win->tocKeepSelection) {
        return;
    }

    auto treeView = win->tocTreeView;
    auto item = TreeItemForPageNo(treeView, currPageNo);
    // only select the items that are visible i.e. are top nodes or
    // children of expanded node
    TreeItem toSelect = (TreeItem)FindVisibleParentTreeItem(treeView, item);
    treeView->SelectItem(toSelect);
}

static void UpdateDocTocExpansionStateRecur(TreeView* treeView, Vec<int>& tocState, TocItem* tocItem) {
    while (tocItem) {
        // items without children cannot be toggled
        if (tocItem->child) {
            // we have to query the state of the tree view item because
            // isOpenToggled is not kept in sync
            // TODO: keep toggle state on TocItem in sync
            // by subscribing to the right notifications
            bool isExpanded = treeView->IsExpanded((TreeItem)tocItem);
            bool wasToggled = isExpanded != tocItem->isOpenDefault;
            if (wasToggled) {
                tocState.Append(tocItem->id);
            }
            UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem->child);
        }
        tocItem = tocItem->next;
    }
}

void UpdateTocExpansionState(Vec<int>& tocState, TreeView* treeView, TocTree* docTree) {
    if (treeView->treeModel != docTree) {
        // CrashMe();
        return;
    }
    tocState.Reset();
    TocItem* tocItem = docTree->root->child;
    UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem);
}

static bool inRange(WCHAR c, WCHAR low, WCHAR hi) {
    return (low <= c) && (c <= hi);
}

// copied from mupdf/fitz/dev_text.c
// clang-format off
static bool isLeftToRightChar(WCHAR c) {
    return (
        inRange(c, 0x0041, 0x005A) ||
        inRange(c, 0x0061, 0x007A) ||
        inRange(c, 0xFB00, 0xFB06)
    );
}

static bool isRightToLeftChar(WCHAR c) {
    return (
        inRange(c, 0x0590, 0x05FF) ||
        inRange(c, 0x0600, 0x06FF) ||
        inRange(c, 0x0750, 0x077F) ||
        inRange(c, 0xFB50, 0xFDFF) ||
        inRange(c, 0xFE70, 0xFEFE)
    );
}
// clang-format off

static void GetLeftRightCounts(TocItem* node, int& l2r, int& r2l) {
next:
    if (!node) {
        return;
    }
    // short-circuit because this could overflow the stack due to recursion
    // (happened in doc from https://github.com/sumatrapdfreader/sumatrapdf/issues/1795)
    if (l2r + r2l > 1024) {
        return;
    }
    if (node->title) {
        TempWStr ws = ToWStrTemp(node->title);
        for (const WCHAR* c = ws; *c; c++) {
            if (isLeftToRightChar(*c)) {
                l2r++;
            } else if (isRightToLeftChar(*c)) {
                r2l++;
            }
        }
    }
    GetLeftRightCounts(node->child, l2r, r2l);
    // could be: GetLeftRightCounts(node->next, l2r, r2l);
    // but faster if not recursive
    node = node->next;
    goto next;
}

static void SetInitialExpandState(TocItem* item, Vec<int>& tocState) {
    while (item) {
        item->isOpenToggled = tocState.Contains(item->id);
        SetInitialExpandState(item->child, tocState);
        item = item->next;
    }
}

static void AddFavoriteFromToc(MainWindow* win, TocItem* dti) {
    int pageNo = 0;
    if (!dti) {
        return;
    }
    if (dti->dest) {
        pageNo = PageDestGetPageNo(dti->dest);
    }
    char* name = dti->title;
    TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
    AddFavoriteWithLabelAndName(win, pageNo, pageLabel, name);
}

static void SaveAttachment(WindowTab* tab, const char* fileName, int attachmentNo) {
    EngineBase* engine = tab->AsFixed()->GetEngine();
    ByteSlice data = EngineMupdfLoadAttachment(engine, attachmentNo);
    if (data.empty()) {
        return;
    }
    char* dir = path::GetDirTemp(tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data.data());
}

static void OpenAttachment(WindowTab* tab, const char* fileName, int attachmentNo) {
    EngineBase* engine = tab->AsFixed()->GetEngine();
    ByteSlice data = EngineMupdfLoadAttachment(engine, attachmentNo);
    if (data.empty()) {
        return;
    }
    MainWindow* win = tab->win;
    EngineBase* newEngine = CreateEngineMupdfFromData(data, fileName, nullptr);
    DocController* ctrl = CreateControllerForEngineOrFile(newEngine, nullptr, nullptr, win);
    LoadArgs* args = new LoadArgs(tab->filePath, win);    
    args->ctrl = ctrl;
    LoadDocumentFinish(args);
    str::Free(data.data());
}

static void OpenEmbeddedFile(WindowTab* tab, IPageDestination* dest) {
    ReportIf(!tab || !dest);
    if (!tab || !dest) {
        return;
    }
    MainWindow* win = tab->win;
    PageDestinationFile *destFile = (PageDestinationFile*)dest;
    char* path = destFile->path;
    const char* tabPath = tab->filePath;
    if (!str::StartsWith(path, tabPath)) {
        return;
    }
    LoadArgs args(path, win);
    args.activateExisting = true;
    LoadDocument(&args);
}

static void SaveEmbeddedFile(WindowTab* tab, const char* srcPath, const char* fileName) {
    ByteSlice data = LoadEmbeddedPDFFile(srcPath);
    if (data.empty()) {
        // TODO: show an error message
        return;
    }
    char* dir = path::GetDirTemp(tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data.data());
}


static void NotifyPdfBookmarksChanged(MainWindow* win) {
    if (!win) {
        return;
    }

    ToolbarUpdateStateForWindow(win, false);

    if (win->tocTreeView && win->tocTreeView->hwnd) {
        InvalidateRect(win->tocTreeView->hwnd, nullptr, TRUE);
    }
}

static TempStr CleanBookmarkTitleTemp(const char* s);
bool SavePdfBookmarksToExistingFile(WindowTab* tab);
bool SavePdfBookmarksToMaybeNewPdfFile(WindowTab* tab);
// clang-format off
static MenuDef menuDefContextToc[] = {
    {
        _TRN("Expand All"),
        CmdExpandAll,
    },
    {
        _TRN("Collapse All"),
        CmdCollapseAll,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Add Bookmark After This"),
        CmdPdfAddBookmark,
    },
    {
        _TRN("Add Child Bookmark"),
        CmdPdfAddChildBookmark,
    },
    {
        _TRN("Rename Bookmark"),
        CmdPdfRenameBookmark,
    },
    {
        _TRN("Delete Bookmark"),
        CmdPdfDeleteBookmark,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Move Bookmark Up"),
        CmdPdfMoveBookmarkUp,
    },
    {
        _TRN("Move Bookmark Down"),
        CmdPdfMoveBookmarkDown,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Save Bookmark Changes"),
        CmdPdfSaveBookmarks,
    },
    {
        _TRN("Save Bookmark Changes As..."),
        CmdPdfSaveBookmarksNewFile,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Open Embedded PDF"),
        CmdOpenEmbeddedPDF,
    },
    {
        _TRN("Save Embedded File..."),
        CmdSaveEmbeddedFile,
    },
    {
        _TRN("Open Attachment"),
        CmdOpenAttachment,
    },
    {
        _TRN("Save Attachment..."),
        CmdSaveAttachment,
    },
    {
        "Add to favorites",
        CmdFavoriteAdd,
    },
    {
        "Remove from favorites",
        CmdFavoriteDel,
    },
    {
        nullptr,
        0,
    },
};
// clang-format on

////////////////// BOOKMARKS ///////////////////////////////
static bool BeginRenameTocBookmark(MainWindow* win, TocItem* item);
static bool CommitRenameTocBookmark(MainWindow* win, NMTVDISPINFOW* info);
static bool DeletePdfBookmark(MainWindow* win, TocItem* item);
static bool AddPdfBookmarkAfterTocItem(MainWindow* win, TocItem* afterItem);
static bool AddChildPdfBookmarkToTocItem(MainWindow* win, TocItem* parentItem);
static bool MovePdfBookmark(MainWindow* win, TocItem* item, bool moveDown);

static bool SelectionIsOnlyOnPage(WindowTab* tab, int pageNo) {
    if (!tab || !tab->selectionOnPage || tab->selectionOnPage->size() == 0) {
        return false;
    }

    for (SelectionOnPage& sel : *tab->selectionOnPage) {
        if (sel.pageNo != pageNo) {
            return false;
        }
    }

    return true;
}

static TempStr GetBookmarkTitleForCurrentSelectionTemp(WindowTab* tab, int pageNo) {
    if (!SelectionIsOnlyOnPage(tab, pageNo)) {
        return nullptr;
    }

    bool isTextOnlySelection = false;
    TempStr selectedText = GetSelectedTextTemp(tab, " ", isTextOnlySelection);

    // Avoid using rectangular/image-style selections as bookmark titles.
    if (!isTextOnlySelection) {
        return nullptr;
    }

    return CleanBookmarkTitleTemp(selectedText);
}

static TempStr GetDefaultPdfBookmarkTitleTemp(WindowTab* tab, int pageNo) {
    TempStr selectedTitle = GetBookmarkTitleForCurrentSelectionTemp(tab, pageNo);
    if (!str::IsEmpty(selectedTitle)) {
        return selectedTitle;
    }

    return str::FormatTemp("Page %d", pageNo);
}

static bool AddPdfBookmarkAfterTocItem(MainWindow* win, TocItem* afterItem) {
    if (!win || !win->ctrl || !afterItem) {
        return false;
    }

    if (HasTocFilter(win)) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    int pageNo = win->ctrl->CurrentPageNo();
    if (pageNo <= 0) {
        return false;
    }

    WindowTab* tab = win->CurrentTab();
    TempStr title = GetDefaultPdfBookmarkTitleTemp(tab, pageNo);

    PdfBookmarkEditor editor(engine);
    if (!editor.AddBookmarkAfter(afterItem, title, pageNo)) {
        return false;
    }

    TreeView* treeView = win->tocTreeView;
    if (treeView && treeView->hwnd) {
        treeView->SetTreeModel(treeView->treeModel);
        treeView->SelectItem((TreeItem)afterItem->next);
        InvalidateRect(treeView->hwnd, nullptr, TRUE);
    }

    NotifyPdfBookmarksChanged(win);
    return true;
}

static bool BeginRenameTocBookmark(MainWindow* win, TocItem* item) {
    if (!win || !win->tocTreeView || !item) {
        return false;
    }

    if (HasTocFilter(win)) {
        return false;
    }

    HWND hwndTree = win->tocTreeView->hwnd;
    if (!hwndTree) {
        return false;
    }

    HTREEITEM hItem = item->hItem;
    if (!hItem) {
        hItem = TreeView_GetSelection(hwndTree);
    }

    if (!hItem) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(hwndTree, GWL_STYLE);
    SetWindowLongPtrW(hwndTree, GWL_STYLE, style | (LONG_PTR)TVS_EDITLABELS);

    SetFocus(hwndTree);

    HWND hwndEdit = TreeView_EditLabel(hwndTree, hItem);
    return hwndEdit != nullptr;
}

// Commits native TreeView label-edit text through PdfBookmarkEditor.
static bool CommitRenameTocBookmark(MainWindow* win, NMTVDISPINFOW* info) {
    if (!win || !info) {
        return false;
    }

    // User canceled editing, usually with Esc.
    if (!info->item.pszText) {
        return false;
    }

    TocItem* item = (TocItem*)info->item.lParam;
    if (!item) {
        return false;
    }

    TempStr newTitle = ToUtf8Temp(info->item.pszText);
    if (!newTitle) {
        return false;
    }

    // Reject empty or whitespace-only titles.
    const char* s = newTitle;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    if (*s == '\0') {
        return false;
    }

    // No-op rename. Accept so the edit box closes cleanly.
    if (item->title && str::Eq(item->title, newTitle)) {
        return true;
    }

    EngineBase* engine = nullptr;
    DisplayModel* dm = win->AsFixed();
    if (dm) {
        engine = dm->GetEngine();
    }

    PdfBookmarkEditor editor(engine);
    bool ok = editor.RenameBookmark(item, newTitle);
    if (!ok) {
        return false;
    }

    NotifyPdfBookmarksChanged(win);
    return true;
}

static bool AddRootPdfBookmark(MainWindow* win) {
    if (!win || !win->ctrl) {
        return false;
    }

    if (HasTocFilter(win)) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    int pageNo = win->ctrl->CurrentPageNo();
    if (pageNo <= 0) {
        return false;
    }

    WindowTab* tab = win->CurrentTab();
    TempStr title = GetDefaultPdfBookmarkTitleTemp(tab, pageNo);

    PdfBookmarkEditor editor(engine);
    if (!editor.AddRootBookmark(title, pageNo)) {
        return false;
    }

    TreeView* treeView = win->tocTreeView;
    if (treeView && treeView->hwnd) {
        TocTree* model = tab ? tab->currToc : nullptr;
        if (!model) {
            model = editor.GetOrCreateTocTree();
            if (tab) {
                tab->currToc = model;
            }
        }

        treeView->Clear();
        treeView->SetTreeModel(model);

        if (model && model->root && model->root->child) {
            TocItem* item = model->root->child;
            while (item->next) {
                item = item->next;
            }
            treeView->SelectItem((TreeItem)item);
        }

        InvalidateRect(treeView->hwnd, nullptr, TRUE);
    }

    NotifyPdfBookmarksChanged(win);
    return true;
}

static void TocContextMenu(ContextMenuEvent* ev) {
    if (!ev || !ev->w) {
        return;
    }

    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    if (!win || !win->ctrl) {
        return;
    }

    POINT pt{};
    TreeItem ti = GetOrSelectTreeItemAtPos(ev, pt);
    if (ti == TreeModel::kNullItem) {
        pt = {ev->mouseScreen.x, ev->mouseScreen.y};
    }

    TocItem* dti = nullptr;
    if (ti != TreeModel::kNullItem) {
        dti = (TocItem*)ti;
    }

    WindowTab* tab = win->CurrentTab();

    EngineBase* engine = nullptr;
    if (tab && tab->AsFixed()) {
        engine = tab->AsFixed()->GetEngine();
    }

    const bool filterActive = HasTocFilter(win);

    bool canEditPdfBookmarks = false;
    bool canRenamePdfBookmark = false;
    bool canAddPdfBookmark = false;
    bool canDeletePdfBookmark = false;
    bool canAddChildPdfBookmark = false;
    bool canMovePdfBookmarkUp = false;
    bool canMovePdfBookmarkDown = false;

    if (!filterActive && engine) {
        PdfBookmarkEditor editor(engine);

        canEditPdfBookmarks = editor.CanEditBookmarks();
        canAddPdfBookmark = canEditPdfBookmarks && editor.CanAddBookmarks();

        if (dti) {
            canRenamePdfBookmark = canEditPdfBookmarks && editor.CanRenameBookmarks();
            canAddChildPdfBookmark = canEditPdfBookmarks && editor.CanAddBookmarks();
            canDeletePdfBookmark = canEditPdfBookmarks && editor.CanDeleteBookmarks();
            canMovePdfBookmarkUp = canEditPdfBookmarks && editor.CanMoveBookmarkUp(dti);
            canMovePdfBookmarkDown = canEditPdfBookmarks && editor.CanMoveBookmarkDown(dti);
        }
    }

    bool hasUnsavedPdfBookmarks = false;
    if (engine) {
        hasUnsavedPdfBookmarks = EngineMupdfHasUnsavedPdfBookmarks(engine);
    }

    int pageNo = 0;
    IPageDestination* dest = dti ? dti->dest : nullptr;
    if (dest) {
        pageNo = PageDestGetPageNo(dest);
    }

    const char* filePath = win->ctrl->GetFilePath();

    HMENU popup = BuildMenuFromDef(menuDefContextToc, CreatePopupMenu(), nullptr);
    if (!popup) {
        return;
    }

    if (!canAddPdfBookmark) {
        MenuRemove(popup, CmdPdfAddBookmark);
    }
    if (canAddPdfBookmark) {
        MenuSetText(popup, CmdPdfAddBookmark, dti ? _TRA("Add Bookmark After This") : _TRA("Add Bookmark Here"));
    }

    if (!canAddChildPdfBookmark) {
        MenuRemove(popup, CmdPdfAddChildBookmark);
    }

    if (!canRenamePdfBookmark) {
        MenuRemove(popup, CmdPdfRenameBookmark);
    }

    if (!canDeletePdfBookmark) {
        MenuRemove(popup, CmdPdfDeleteBookmark);
    }

    if (!canMovePdfBookmarkUp) {
        MenuRemove(popup, CmdPdfMoveBookmarkUp);
    }

    if (!canMovePdfBookmarkDown) {
        MenuRemove(popup, CmdPdfMoveBookmarkDown);
    }

    if (!hasUnsavedPdfBookmarks) {
        MenuRemove(popup, CmdPdfSaveBookmarks);
        MenuRemove(popup, CmdPdfSaveBookmarksNewFile);
    }

    const char* path = nullptr;
    char* fileName = nullptr;
    Kind destKind = dest ? dest->GetKind() : nullptr;

    if (destKind == kindDestinationLaunchEmbedded) {
        auto embeddedFile = (PageDestinationFile*)dest;
        path = embeddedFile->path;
        fileName = PageDestGetName(dest);

        bool canOpenEmbedded = fileName && str::EndsWithI(fileName, ".pdf");
        if (!canOpenEmbedded) {
            MenuRemove(popup, CmdOpenEmbeddedPDF);
        }
    } else {
        MenuRemove(popup, CmdSaveEmbeddedFile);
        MenuRemove(popup, CmdOpenEmbeddedPDF);
    }

    int attachmentNo = -1;
    if (destKind == kindDestinationAttachment) {
        auto attachment = (PageDestinationFile*)dest;
        path = attachment->path;
        fileName = PageDestGetName(dest);

        // attachmentNo is saved in pageNo. See PdfLoadAttachments and DestFromAttachment.
        attachmentNo = pageNo;

        bool canOpenAttachment = fileName && str::EndsWithI(fileName, ".pdf");
        if (!canOpenAttachment) {
            MenuRemove(popup, CmdOpenAttachment);
        }
    } else {
        MenuRemove(popup, CmdSaveAttachment);
        MenuRemove(popup, CmdOpenAttachment);
    }

    if (pageNo > 0 && filePath) {
        TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
        bool isBookmarked = IsPageInFavorites(filePath, pageNo);

        if (isBookmarked) {
            MenuRemove(popup, CmdFavoriteAdd);

            const char* tr = _TRA("Remove page %s from favorites");
            TempStr s = str::FormatTemp(tr, pageLabel);
            MenuSetText(popup, CmdFavoriteDel, s);
        } else {
            MenuRemove(popup, CmdFavoriteDel);

            TempStr s = str::FormatTemp(_TRA("Add page %s to favorites"), pageLabel);
            s = AppendAccelKeyToMenuStringTemp(s, CmdFavoriteAdd);
            MenuSetText(popup, CmdFavoriteAdd, s);
        }
    } else {
        MenuRemove(popup, CmdFavoriteAdd);
        MenuRemove(popup, CmdFavoriteDel);
    }

    RemoveBadMenuSeparators(popup);
    MarkMenuOwnerDraw(popup);

    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmd = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);

    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);

    switch (cmd) {
        case CmdExpandAll:
            win->tocTreeView->ExpandAll();
            break;

        case CmdCollapseAll:
            win->tocTreeView->CollapseAll();
            break;

        case CmdPdfAddBookmark:
            if (canAddPdfBookmark) {
                if (dti) {
                    AddPdfBookmarkAfterTocItem(win, dti);
                } else {
                    AddRootPdfBookmark(win);
                }
            }
            break;

        case CmdPdfAddChildBookmark:
            if (canAddChildPdfBookmark) {
                AddChildPdfBookmarkToTocItem(win, dti);
            }
            break;

        case CmdPdfRenameBookmark:
            if (canRenamePdfBookmark) {
                BeginRenameTocBookmark(win, dti);
            }
            break;

        case CmdPdfDeleteBookmark:
            if (canDeletePdfBookmark) {
                DeletePdfBookmark(win, dti);
            }
            break;

        case CmdPdfMoveBookmarkUp:
            if (canMovePdfBookmarkUp) {
                MovePdfBookmark(win, dti, false);
            }
            break;

        case CmdPdfMoveBookmarkDown:
            if (canMovePdfBookmarkDown) {
                MovePdfBookmark(win, dti, true);
            }
            break;

        case CmdPdfSaveBookmarks:
            if (tab && hasUnsavedPdfBookmarks) {
                SavePdfBookmarksToExistingFile(tab);
            }
            break;

        case CmdPdfSaveBookmarksNewFile:
            if (tab && hasUnsavedPdfBookmarks) {
                SavePdfBookmarksToMaybeNewPdfFile(tab);
            }
            break;

        case CmdFavoriteAdd:
            if (filePath && pageNo > 0 && dti) {
                AddFavoriteFromToc(win, dti);
            }
            break;

        case CmdFavoriteDel:
            if (filePath && pageNo > 0) {
                DelFavorite(filePath, pageNo);
            }
            break;

        case CmdSaveEmbeddedFile:
            if (tab && path && fileName) {
                SaveEmbeddedFile(tab, path, fileName);
            }
            break;

        case CmdOpenEmbeddedPDF:
            if (tab && dest) {
                OpenEmbeddedFile(tab, dest);
            }
            break;

        case CmdSaveAttachment:
            if (tab && fileName && attachmentNo >= 0) {
                SaveAttachment(tab, fileName, attachmentNo);
            }
            break;

        case CmdOpenAttachment:
            if (tab && fileName && attachmentNo >= 0) {
                OpenAttachment(tab, fileName, attachmentNo);
            }
            break;
    }
}

static bool AddChildPdfBookmarkToTocItem(MainWindow* win, TocItem* parentItem) {
    if (!win || !win->ctrl || !parentItem) {
        return false;
    }

    if (HasTocFilter(win)) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    int pageNo = win->ctrl->CurrentPageNo();
    if (pageNo <= 0) {
        return false;
    }

    WindowTab* tab = win->CurrentTab();
    TempStr title = GetDefaultPdfBookmarkTitleTemp(tab, pageNo);

    PdfBookmarkEditor editor(engine);
    if (!editor.AddChildBookmark(parentItem, title, pageNo)) {
        return false;
    }

    TreeView* treeView = win->tocTreeView;
    if (treeView && treeView->hwnd) {
        treeView->SetTreeModel(treeView->treeModel);
        treeView->SelectItem((TreeItem)parentItem->child);
        InvalidateRect(treeView->hwnd, nullptr, TRUE);
    }

    NotifyPdfBookmarksChanged(win);
    return true;
}

static TempStr CleanBookmarkTitleTemp(const char* s) {
    if (str::IsEmpty(s)) {
        return nullptr;
    }

    StrBuilder title;
    bool pendingSpace = false;

    for (const char* p = s; *p; p++) {
        if (str::IsWs(*p)) {
            pendingSpace = title.Size() > 0;
            continue;
        }

        if (pendingSpace) {
            title.AppendChar(' ');
            pendingSpace = false;
        }

        title.AppendChar(*p);

        // Keep bookmark labels readable. Long selected paragraphs make bad TOC entries.
        if (title.Size() >= 120) {
            title.Append("...");
            break;
        }
    }

    if (title.Size() == 0) {
        return nullptr;
    }

    return str::DupTemp(title.CStr());
}

static int CountTocItemDescendants(TocItem* item) {
    if (!item) {
        return 0;
    }

    int count = 0;

    for (TocItem* child = item->child; child; child = child->next) {
        count++;
        count += CountTocItemDescendants(child);
    }

    return count;
}

static bool ConfirmDeletePdfBookmark(MainWindow* win, TocItem* item) {
    if (!win || !item) {
        return false;
    }

    const char* title = item->title ? item->title : "";
    int childCount = CountTocItemDescendants(item);

    TempStr msg = nullptr;
    if (childCount > 0) {
        msg = str::FormatTemp(_TRA("Delete bookmark \"%s\" and its %d child bookmarks?"), title, childCount);
    } else {
        msg = str::FormatTemp(_TRA("Delete bookmark \"%s\"?"), title);
    }

    TempWStr msgW = ToWStrTemp(msg);
    TempWStr titleW = ToWStrTemp(_TRA("Delete Bookmark"));

    int res = MessageBoxW(win->hwndFrame, msgW, titleW, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    return res == IDYES;
}

static bool DeletePdfBookmark(MainWindow* win, TocItem* item) {
    if (!win || !win->ctrl || !win->tocTreeView || !item) {
        return false;
    }

    if (HasTocFilter(win)) {
        return false;
    }

    if (!ConfirmDeletePdfBookmark(win, item)) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    TocItem* itemToSelect = item->next;

    PdfBookmarkEditor editor(engine);
    if (!editor.DeleteBookmark(item)) {
        return false;
    }

    TreeView* treeView = win->tocTreeView;
    if (treeView && treeView->hwnd) {
        TreeModel* model = treeView->treeModel;

        treeView->Clear();
        treeView->SetTreeModel(model);

        if (itemToSelect) {
            treeView->SelectItem((TreeItem)itemToSelect);
        }

        InvalidateRect(treeView->hwnd, nullptr, TRUE);
    }

    NotifyPdfBookmarksChanged(win);
    return true;
}

static bool MovePdfBookmark(MainWindow* win, TocItem* item, bool moveDown) {
    if (!win || !win->ctrl || !win->tocTreeView || !item) {
        return false;
    }

    if (HasTocFilter(win)) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    PdfBookmarkEditor editor(engine);

    bool ok = moveDown ? editor.MoveBookmarkDown(item) : editor.MoveBookmarkUp(item);
    if (!ok) {
        return false;
    }

    TreeView* treeView = win->tocTreeView;
    if (treeView && treeView->hwnd) {
        TreeModel* model = treeView->treeModel;

        treeView->Clear();
        treeView->SetTreeModel(model);
        treeView->SelectItem((TreeItem)item);

        InvalidateRect(treeView->hwnd, nullptr, TRUE);
    }

    NotifyPdfBookmarksChanged(win);
    return true;
}

static bool CanAddRootPdfBookmarkForCurrentPage(MainWindow* win) {
    if (!win || !win->ctrl) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    int pageNo = win->ctrl->CurrentPageNo();
    if (pageNo <= 0) {
        return false;
    }

    PdfBookmarkEditor editor(engine);
    return editor.CanEditBookmarks() && editor.CanAddBookmarks();
}

static void ClearTocFilterIfAny(MainWindow* win) {
    if (!win || !win->tocFilterEdit) {
        return;
    }

    if (!HasTocFilter(win)) {
        return;
    }

    win->tocFilterEdit->SetText("");
}

bool AddPdfBookmarkForCurrentPageAndShowToc(MainWindow* win) {
    if (!CanAddRootPdfBookmarkForCurrentPage(win)) {
        return false;
    }

    ClearTocFilterIfAny(win);

    if (!win->tocVisible) {
        SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    }

    if (win->tocVisible && !win->tocLoaded) {
        LoadTocTree(win);
    }

    if (!AddRootPdfBookmark(win)) {
        return false;
    }

    if (win->tocVisible && win->tocTreeView && win->tocTreeView->hwnd) {
        HwndSetFocus(win->tocTreeView->hwnd);
    }

    return true;
}

void OnTocCustomDraw(TreeView::CustomDrawEvent*);

// auto-expand root level ToC nodes if there are at most two
static void AutoExpandTopLevelItems(TocItem* root) {
    if (!root) {
        return;
    }
    if (root->next && root->next->next) {
        return;
    }

    if (!root->IsExpanded()) {
        root->isOpenToggled = !root->isOpenToggled;
    }
    if (!root->next) {
        return;
    }
    if (!root->next->IsExpanded()) {
        root->next->isOpenToggled = !root->next->isOpenToggled;
    }
}

void LoadTocTree(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    ReportIf(!tab);

    if (win->tocLoaded) {
        return;
    }

    // clear filter when loading new toc
    // null out currToc first so that SetText("") callback doesn't use stale pointer
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;
    tab->currToc = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    auto* tocTree = tab->ctrl->GetToc();

    if (!tocTree || !tocTree->root) {
        DisplayModel* dm = win->AsFixed();
        EngineBase* engine = dm ? dm->GetEngine() : nullptr;

        PdfBookmarkEditor editor(engine);
        tocTree = editor.GetOrCreateTocTree();
    }

    if (!tocTree || !tocTree->root) {
        return;
    }

    tab->currToc = tocTree;

    // consider a ToC tree right-to-left if a more than half of the
    // alphabetic characters are in a right-to-left script
    int l2r = 0, r2l = 0;
    GetLeftRightCounts(tocTree->root, l2r, r2l);
    bool isRTL = r2l > l2r;

    TreeView* treeView = win->tocTreeView;
    HWND hwnd = treeView->hwnd;
    HwndSetRtl(hwnd, isRTL);

    UpdateControlsColors(win);
    SetInitialExpandState(tocTree->root, tab->tocState);
    AutoExpandTopLevelItems(tocTree->root->child);

    treeView->SetTreeModel(tocTree);

    treeView->onCustomDraw = MkFunc1Void(OnTocCustomDraw);
    LayoutTocContainer(win);
    // uint fl = RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN;
    // RedrawWindow(hwnd, nullptr, nullptr, fl);

    win->tocLoaded = true;
}

// TODO: use https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getobject?redirectedfrom=MSDN
// to get LOGFONT from existing font and then create a derived font
static void UpdateFont(HDC hdc, int fontFlags) {
    // TODO: this is a bit hacky, in that we use default font
    // and not the font from TreeCtrl. But in this case they are the same
    bool italic = bit::IsSet(fontFlags, fontBitItalic);
    bool bold = bit::IsSet(fontFlags, fontBitBold);
    HFONT hfont = GetDefaultGuiFont(bold, italic);
    SelectObject(hdc, hfont);
}

static bool HasTocFilter(MainWindow* win) {
    if (!win || !win->tocFilterEdit) {
        return false;
    }
    TempStr filter = win->tocFilterEdit->GetTextTemp();
    return filter && str::Len(filter) > 0;
}

static void DrawTocItemHighlight(TreeView::CustomDrawEvent* ev, MainWindow* win) {
    TocItem* tocItem = (TocItem*)ev->treeItem;
    if (!tocItem || !tocItem->title) {
        return;
    }
    Edit* edit = win->tocFilterEdit;
    if (!edit) {
        return;
    }
    TempStr filter = edit->GetTextTemp();
    if (!filter || str::Len(filter) == 0) {
        return;
    }
    const char* title = tocItem->title;
    int titleLen = str::Leni(title);
    if (titleLen == 0) {
        return;
    }

    // mark which bytes are part of a match
    u8* highlighted = AllocArrayTemp<u8>(titleLen);
    int filterLen = str::Leni(filter);
    const char* p = title;
    while ((p = str::FindI(p, filter)) != nullptr) {
        int off = (int)(p - title);
        for (int k = 0; k < filterLen && off + k < titleLen; k++) {
            highlighted[off + k] = 1;
        }
        p += filterLen;
    }

    // collect contiguous highlighted ranges (up to 16)
    struct ByteRange {
        int start;
        int end;
    };
    ByteRange byteRanges[16];
    int nRanges = 0;
    {
        int pos = 0;
        while (pos < titleLen && nRanges < 16) {
            if (highlighted[pos]) {
                int start = pos;
                while (pos < titleLen && highlighted[pos]) {
                    pos++;
                }
                byteRanges[nRanges++] = {start, pos};
            } else {
                pos++;
            }
        }
    }
    if (nRanges == 0) {
        return;
    }

    // get the label rect for this tree item
    RECT labelRect;
    TreeView* tv = ev->treeView;
    if (!tv->GetItemRect(ev->treeItem, true, labelRect)) {
        return;
    }

    NMTVCUSTOMDRAW* tvcd = ev->nm;
    HDC hdc = tvcd->nmcd.hdc;

    WCHAR* titleW = ToWStrTemp(title);

    // compute pixel rectangles for each highlighted range
    RECT highlightRects[16];
    for (int i = 0; i < nRanges; i++) {
        WCHAR* prefixToStart = ToWStrTemp(title, (size_t)byteRanges[i].start);
        int wStart = str::Leni(prefixToStart);
        WCHAR* prefixToEnd = ToWStrTemp(title, (size_t)byteRanges[i].end);
        int wEnd = str::Leni(prefixToEnd);

        SIZE szStart, szEnd;
        GetTextExtentPoint32W(hdc, titleW, wStart, &szStart);
        GetTextExtentPoint32W(hdc, titleW, wEnd, &szEnd);

        highlightRects[i].top = labelRect.top;
        highlightRects[i].bottom = labelRect.bottom;
        highlightRects[i].left = labelRect.left + szStart.cx;
        highlightRects[i].right = labelRect.left + szEnd.cx;
    }

    // erase the label area with the correct background color
    // so we can redraw text cleanly without double-draw artifacts
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
    bool hasFocus = (GetFocus() == tv->hwnd);
    COLORREF bgCol;
    if (isSelected) {
        bgCol = GetSysColor(hasFocus ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
    } else {
        bgCol = IsSpecialColor(tv->bgColor) ? GetSysColor(COLOR_WINDOW) : tv->bgColor;
    }
    HBRUSH hbrBg = CreateSolidBrush(bgCol);
    FillRect(hdc, &labelRect, hbrBg);
    DeleteObject(hbrBg);

    // draw highlight background rectangles
    COLORREF highlightCol;
    if (IsCurrentThemeDefault()) {
        highlightCol = RGB(255, 255, 0);
    } else {
        highlightCol = AccentColor(bgCol, 40);
    }
    HBRUSH hbrHighlight = CreateSolidBrush(highlightCol);
    for (int i = 0; i < nRanges; i++) {
        FillRect(hdc, &highlightRects[i], hbrHighlight);
    }
    DeleteObject(hbrHighlight);

    // draw the text on top
    COLORREF txtCol;
    if (isSelected && hasFocus) {
        txtCol = GetSysColor(COLOR_HIGHLIGHTTEXT);
    } else if (tocItem->color != kColorUnset) {
        txtCol = tocItem->color;
    } else {
        txtCol = IsSpecialColor(tv->textColor) ? GetSysColor(COLOR_WINDOWTEXT) : tv->textColor;
    }
    COLORREF oldTxtCol = SetTextColor(hdc, txtCol);
    int oldBkMode = SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, titleW, -1, &labelRect, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetBkMode(hdc, oldBkMode);
    SetTextColor(hdc, oldTxtCol);
}

// https://docs.microsoft.com/en-us/windows/win32/controls/about-custom-draw
// https://docs.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-nmtvcustomdraw
void OnTocCustomDraw(TreeView::CustomDrawEvent* ev) {
#if defined(DISPLAY_TOC_PAGE_NUMBERS)
    if (false) return CDRF_DODEFAULT;
    switch (((LPNMCUSTOMDRAW)pnmtv)->dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_DODEFAULT | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT:
            RelayoutTocItem((LPNMTVCUSTOMDRAW)pnmtv);
            // fall through
        default:
            return CDRF_DODEFAULT;
    }
    break;
#endif

    ev->result = CDRF_DODEFAULT;
    NMTVCUSTOMDRAW* tvcd = ev->nm;
    NMCUSTOMDRAW* cd = &(tvcd->nmcd);

    if (cd->dwDrawStage == CDDS_PREPAINT) {
        ev->result = CDRF_NOTIFYITEMDRAW;
        return;
    }

    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    bool filterActive = HasTocFilter(win);

    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        TocItem* tocItem = (TocItem*)ev->treeItem;
        if (!tocItem) {
            return;
        }
        LRESULT res = 0;
        if (tocItem->color != kColorUnset) {
            tvcd->clrText = tocItem->color;
        }
        if (tocItem->fontFlags != 0) {
            UpdateFont(cd->hdc, tocItem->fontFlags);
            res = CDRF_NEWFONT;
        }
        if (filterActive) {
            res |= CDRF_NOTIFYPOSTPAINT;
        }
        ev->result = res;
        return;
    }

    if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
        if (filterActive && win) {
            DrawTocItemHighlight(ev, win);
        }
        ev->result = CDRF_DODEFAULT;
        return;
    }
}

// disabled becaues of https://github.com/sumatrapdfreader/sumatrapdf/issues/2202
// it was added for https://github.com/sumatrapdfreader/sumatrapdf/issues/1716
// but unclear if its still needed
// this calls GoToTocLinkTask) which will eventually call GoToPage()
// which adds nav point. Maybe I should not add nav point
// if going to the same page?
void TocTreeClick(TreeView::ClickEvent* ev) {
#if 0
    ev->didHandle = true;
    if (!ev->treeItem) {
        return;
    }
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    ReportIf(!win);
    bool allowExternal = false;
    GoToTocTreeItem(win, ev->treeItem, allowExternal);
#endif
    ev->result = -1;
}

static void TocTreeSelectionChanged(TreeView::SelectionChangedEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    ReportIf(!win);

    // When the focus is set to the toc window the first item in the treeview is automatically
    // selected and a TVN_SELCHANGEDW notification message is sent with the special code pnmtv->action ==
    // 0x00001000. We have to ignore this message to prevent the current page to be changed.
    // The case pnmtv->action==TVC_UNKNOWN is ignored because
    // it corresponds to a notification sent by
    // the function TreeView_DeleteAllItems after deletion of the item.
    bool shouldHandle = ev->byKeyboard || ev->byMouse;
    if (!shouldHandle) {
        return;
    }
    bool allowExternal = ev->byMouse;
    GoToTocTreeItem(win, ev->selectedItem, allowExternal);
}

void TocTreeKeyDown2(TreeView::KeyDownEvent* ev) {
    // TODO: trying to fix https://github.com/sumatrapdfreader/sumatrapdf/issues/1841
    // doesn't work i.e. page up / page down seems to be processed anyway by TreeCtrl
#if 0
    if ((ev->keyCode == VK_PRIOR) || (ev->keyCode == VK_NEXT)) {
        // up/down in tree is not very useful, so instead
        // send it to frame so that it scrolls document instead
        MainWindow* win = FindMainWindowByHwnd(ev->hwnd);
        // this is sent as WM_NOTIFY to TreeCtrl but for frame it's WM_KEYDOWN
        // alternatively, we could call FrameOnKeydown(ev->wp, ev->lp, false);
        SendMessageW(win->hwndFrame, WM_KEYDOWN, ev->wp, ev->lp);
        ev->didHandle = true;
        ev->result = 1;
        return;
    }
#endif
    if (ev->keyCode != VK_TAB) {
        ev->result = 0;
        return;
    }

    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (win->tabsVisible && IsCtrlPressed()) {
        TabsOnCtrlTab(win, IsShiftPressed());
        ev->result = 1;
        return;
    }
    AdvanceFocus(win);
    ev->result = 1;
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void TocTreeMsgFilter(WndEvent*) {
    switch (msg) {
        case WM_SIZE:
        case WM_HSCROLL:
            // Repaint the ToC so that RelayoutTocItem is called for all items
            PostMessageW(hwnd, WM_APP_REPAINT_TOC, 0, 0);
            break;
        case WM_APP_REPAINT_TOC:
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
            break;
    }
}
#endif

// Position label with close button and tree window within their parent.
// Used for toc and favorites.
void LayoutTreeContainer(LabelWithCloseWnd* l, HWND hwndTree) {
    HWND hwndContainer = GetParent(hwndTree);
    Size labelSize = l->GetIdealSize();
    Rect rc = WindowRect(hwndContainer);
    int dy = rc.dy;
    int y = 0;
    MoveWindow(l->hwnd, y, 0, rc.dx, labelSize.dy, TRUE);
    dy -= labelSize.dy;
    y += labelSize.dy;
    MoveWindow(hwndTree, 0, y, rc.dx, dy, TRUE);
}

// Position label, filter edit, and tree window within toc container.
static void LayoutTocContainer(MainWindow* win) {
    LabelWithCloseWnd* l = win->tocLabelWithClose;
    Edit* edit = win->tocFilterEdit;
    TreeView* treeView = win->tocTreeView;
    HWND hwndContainer = win->hwndTocBox;
    Size labelSize = l->GetIdealSize();
    Rect rc = WindowRect(hwndContainer);
    int dy = rc.dy;
    int y = 0;
    MoveWindow(l->hwnd, 0, y, rc.dx, labelSize.dy, TRUE);
    dy -= labelSize.dy;
    y += labelSize.dy;
    if (edit && edit->hwnd) {
        Size editSize = edit->GetIdealSize();
        MoveWindow(edit->hwnd, 0, y, rc.dx, editSize.dy, TRUE);
        dy -= editSize.dy;
        y += editSize.dy;
    }
    MoveWindow(treeView->hwnd, 0, y, rc.dx, dy, TRUE);
}

static bool GetCurrentTreeClientPoint(HWND hwndTree, POINT* ptTree) {
    if (!hwndTree || !ptTree) {
        return false;
    }

    POINT ptScreen{};
    if (!GetCursorPos(&ptScreen)) {
        return false;
    }

    *ptTree = ptScreen;
    ScreenToClient(hwndTree, ptTree);
    return true;
}

static bool GetTreeClientPointFromScreen(HWND hwndTree, POINT ptScreen, POINT* ptTree) {
    if (!hwndTree || !ptTree) {
        return false;
    }

    *ptTree = ptScreen;
    ScreenToClient(hwndTree, ptTree);
    return true;
}

static TocItem* GetTocItemFromTreeItem(HWND hwndTree, HTREEITEM hItem) {
    if (!hwndTree || !hItem) {
        return nullptr;
    }

    TVITEMW tvItem{};
    tvItem.mask = TVIF_PARAM;
    tvItem.hItem = hItem;

    if (!TreeView_GetItem(hwndTree, &tvItem)) {
        return nullptr;
    }

    return (TocItem*)tvItem.lParam;
}

static TocBookmarkDropMode GetDropModeFromTreePoint(HWND hwndTree, HTREEITEM hItem, POINT ptTree) {
    if (!hwndTree || !hItem) {
        return TocBookmarkDropMode::None;
    }

    RECT rc{};
    if (!TreeView_GetItemRect(hwndTree, hItem, &rc, TRUE)) {
        return TocBookmarkDropMode::None;
    }

    int height = rc.bottom - rc.top;
    if (height <= 0) {
        return TocBookmarkDropMode::None;
    }

    int y = ptTree.y - rc.top;

    if (y < height / 3) {
        return TocBookmarkDropMode::Before;
    }

    if (y > (height * 2) / 3) {
        return TocBookmarkDropMode::After;
    }

    return TocBookmarkDropMode::AsChild;
}

static const char* TocBookmarkDropModeName(TocBookmarkDropMode mode) {
    switch (mode) {
        case TocBookmarkDropMode::Before:
            return "Before";
        case TocBookmarkDropMode::After:
            return "After";
        case TocBookmarkDropMode::AsChild:
            return "AsChild";
        case TocBookmarkDropMode::RootEnd:
            return "RootEnd";
        case TocBookmarkDropMode::None:
        default:
            return "None";
    }
}

static bool IsTocItemAncestorOf(TocItem* maybeAncestor, TocItem* item) {
    if (!maybeAncestor || !item) {
        return false;
    }

    for (TocItem* child = maybeAncestor->child; child; child = child->next) {
        if (child == item) {
            return true;
        }

        if (IsTocItemAncestorOf(child, item)) {
            return true;
        }
    }

    return false;
}

static void ResetTocBookmarkDrag(HWND hwndTree) {
    if (gTocBookmarkDrag.dragImage) {
        if (hwndTree) {
            ImageList_DragLeave(hwndTree);
        }

        ImageList_EndDrag();
        ImageList_Destroy(gTocBookmarkDrag.dragImage);
    }

    if (hwndTree) {
        TreeView_SelectDropTarget(hwndTree, nullptr);
    }

    gTocBookmarkDrag = {};
}

static bool IsPointBelowLastVisibleTreeItem(HWND hwndTree, POINT ptTree) {
    if (!hwndTree) {
        return false;
    }

    HTREEITEM hItem = TreeView_GetRoot(hwndTree);
    if (!hItem) {
        return false;
    }

    HTREEITEM hLast = hItem;

    for (;;) {
        HTREEITEM hNext = TreeView_GetNextItem(hwndTree, hLast, TVGN_NEXTVISIBLE);
        if (!hNext) {
            break;
        }

        hLast = hNext;
    }

    RECT rc{};
    if (!TreeView_GetItemRect(hwndTree, hLast, &rc, FALSE)) {
        return false;
    }

    return ptTree.y > rc.bottom;
}

static TocItem* GetRootTocSiblingList(MainWindow* win) {
    if (!win) {
        return nullptr;
    }

    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->currToc || !tab->currToc->root) {
        return nullptr;
    }

    return tab->currToc->root->child;
}

static int GetTocItemIndexInList(TocItem* first, TocItem* item) {
    if (!first || !item) {
        return -1;
    }

    int index = 0;
    for (TocItem* it = first; it; it = it->next, index++) {
        if (it == item) {
            return index;
        }
    }

    return -1;
}

static TocItem* GetSiblingListForTocItem(MainWindow* win, TocItem* item) {
    if (!win || !item) {
        return nullptr;
    }

    if (item->parent) {
        return item->parent->child;
    }

    return GetRootTocSiblingList(win);
}

static bool AreTocItemsSameLevel(MainWindow* win, TocItem* a, TocItem* b) {
    if (!win || !a || !b) {
        return false;
    }

    if (a->parent != b->parent) {
        return false;
    }

    TocItem* first = GetSiblingListForTocItem(win, a);
    return GetTocItemIndexInList(first, a) >= 0 && GetTocItemIndexInList(first, b) >= 0;
}

static bool MovePdfBookmarkByDrop(MainWindow* win, TocItem* source, TocItem* target, TocBookmarkDropMode mode) {
    if (!win || !source) {
        return false;
    }

    if (mode != TocBookmarkDropMode::RootEnd && !target) {
        return false;
    }

    if (target && source == target) {
        return false;
    }

    if (HasTocFilter(win)) {
        logf("TOC drag move ignored: filter active\n");
        return false;
    }

    if (mode == TocBookmarkDropMode::RootEnd) {
        if (HasTocFilter(win)) {
            logf("TOC drag root move ignored: filter active\n");
            return false;
        }

        DisplayModel* dm = win->AsFixed();
        EngineBase* engine = dm ? dm->GetEngine() : nullptr;

        PdfBookmarkEditor editor(engine);
        if (!editor.MoveBookmarkToRootEnd(source)) {
            logf("TOC drag root move failed: source='%s'\n", source->title ? source->title : "");
            return false;
        }

        TreeView* treeView = win->tocTreeView;
        if (treeView && treeView->hwnd) {
            TreeModel* model = treeView->treeModel;

            treeView->Clear();
            treeView->SetTreeModel(model);
            treeView->SelectItem((TreeItem)source);

            InvalidateRect(treeView->hwnd, nullptr, TRUE);
        }

        NotifyPdfBookmarksChanged(win);

        logf("TOC drag root move done: source='%s'\n", source->title ? source->title : "");
        return true;
    }

    if (mode == TocBookmarkDropMode::AsChild) {
        DisplayModel* dm = win->AsFixed();
        EngineBase* engine = dm ? dm->GetEngine() : nullptr;

        PdfBookmarkEditor editor(engine);
        if (!editor.MoveBookmarkAsChild(source, target)) {
            logf("TOC drag move as child failed: source='%s', target='%s'\n", source->title ? source->title : "",
                 target->title ? target->title : "");
            return false;
        }

        TreeView* treeView = win->tocTreeView;
        if (treeView && treeView->hwnd) {
            TreeModel* model = treeView->treeModel;

            treeView->Clear();
            treeView->SetTreeModel(model);
            treeView->SelectItem((TreeItem)source);

            InvalidateRect(treeView->hwnd, nullptr, TRUE);
        }

        NotifyPdfBookmarksChanged(win);

        logf("TOC drag move as child done: source='%s', target='%s'\n", source->title ? source->title : "",
             target->title ? target->title : "");

        return true;
    }

    if (mode != TocBookmarkDropMode::Before && mode != TocBookmarkDropMode::After) {
        logf("TOC drag move ignored: unsupported mode=%s\n", TocBookmarkDropModeName(mode));
        return false;
    }

    if (!AreTocItemsSameLevel(win, source, target)) {
        logf(
            "TOC drag move ignored: cross-parent before/after not implemented yet, source='%s', target='%s', mode=%s\n",
            source->title ? source->title : "", target->title ? target->title : "", TocBookmarkDropModeName(mode));
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;

    PdfBookmarkEditor editor(engine);
    if (!editor.CanMoveBookmarks()) {
        logf("TOC drag move ignored: CanMoveBookmarks=false\n");
        return false;
    }

    bool moved = false;

    for (;;) {
        TocItem* first = GetSiblingListForTocItem(win, source);
        int sourceIndex = GetTocItemIndexInList(first, source);
        int targetIndex = GetTocItemIndexInList(first, target);

        if (sourceIndex < 0 || targetIndex < 0) {
            logf("TOC drag move failed: sourceIndex=%d targetIndex=%d\n", sourceIndex, targetIndex);
            return false;
        }

        if (mode == TocBookmarkDropMode::Before) {
            if (sourceIndex == targetIndex - 1) {
                break;
            }

            bool ok = false;
            if (sourceIndex < targetIndex) {
                ok = editor.MoveBookmarkDown(source);
            } else {
                ok = editor.MoveBookmarkUp(source);
            }

            if (!ok) {
                logf("TOC drag move failed: source='%s', target='%s', mode=Before\n",
                     source->title ? source->title : "", target->title ? target->title : "");
                return false;
            }

            moved = true;
            continue;
        }

        if (mode == TocBookmarkDropMode::After) {
            if (sourceIndex == targetIndex + 1) {
                break;
            }

            bool ok = false;
            if (sourceIndex < targetIndex) {
                ok = editor.MoveBookmarkDown(source);
            } else {
                ok = editor.MoveBookmarkUp(source);
            }

            if (!ok) {
                logf("TOC drag move failed: source='%s', target='%s', mode=After\n", source->title ? source->title : "",
                     target->title ? target->title : "");
                return false;
            }

            moved = true;
            continue;
        }

        break;
    }

    if (!moved) {
        logf("TOC drag move no-op: source='%s', target='%s', mode=%s\n", source->title ? source->title : "",
             target->title ? target->title : "", TocBookmarkDropModeName(mode));
        return false;
    }

    TreeView* treeView = win->tocTreeView;
    if (treeView && treeView->hwnd) {
        TreeModel* model = treeView->treeModel;

        treeView->Clear();
        treeView->SetTreeModel(model);
        treeView->SelectItem((TreeItem)source);

        InvalidateRect(treeView->hwnd, nullptr, TRUE);
    }

    NotifyPdfBookmarksChanged(win);

    logf("TOC drag move done: source='%s', target='%s', mode=%s\n", source->title ? source->title : "",
         target->title ? target->title : "", TocBookmarkDropModeName(mode));

    return true;
}


static bool BeginTocBookmarkDrag(MainWindow* win, HWND hwndTocBox, NMTREEVIEWW* ntv) {
    if (!win || !hwndTocBox || !ntv || !win->tocTreeView || !win->tocTreeView->hwnd) {
        return false;
    }

    if (HasTocFilter(win)) {
        return false;
    }

    TocItem* item = (TocItem*)ntv->itemNew.lParam;
    if (!item) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;

    PdfBookmarkEditor editor(engine);
    if (!editor.CanEditBookmarks()) {
        return false;
    }

    HWND hwndTree = win->tocTreeView->hwnd;

    ResetTocBookmarkDrag(hwndTree);

    gTocBookmarkDrag.active = true;
    gTocBookmarkDrag.source = item;
    gTocBookmarkDrag.sourceHItem = ntv->itemNew.hItem;
    gTocBookmarkDrag.target = nullptr;
    gTocBookmarkDrag.targetHItem = nullptr;
    gTocBookmarkDrag.mode = TocBookmarkDropMode::None;

    POINT ptTree{};
    GetTreeClientPointFromScreen(hwndTree, ntv->ptDrag, &ptTree);

    gTocBookmarkDrag.dragImage = TreeView_CreateDragImage(hwndTree, ntv->itemNew.hItem);
    if (gTocBookmarkDrag.dragImage) {
        ImageList_BeginDrag(gTocBookmarkDrag.dragImage, 0, 0, 0);
        ImageList_DragEnter(hwndTree, ptTree.x, ptTree.y);
    }

    SetCapture(hwndTocBox);

    logf("TOC drag begin: source='%s' ptTree=(%d,%d)\n", item->title ? item->title : "", ptTree.x, ptTree.y);

    return true;
}

static void UpdateTocBookmarkDrag(MainWindow* win) {
    if (!gTocBookmarkDrag.active || !win || !win->tocTreeView || !win->tocTreeView->hwnd) {
        return;
    }

    HWND hwndTree = win->tocTreeView->hwnd;

    POINT ptTree{};
    if (!GetCurrentTreeClientPoint(hwndTree, &ptTree)) {
        return;
    }

    if (gTocBookmarkDrag.dragImage) {
        ImageList_DragMove(ptTree.x, ptTree.y);
    }

    TVHITTESTINFO hit{};
    hit.pt = ptTree;

    HTREEITEM hTarget = TreeView_HitTest(hwndTree, &hit);
    TocItem* target = GetTocItemFromTreeItem(hwndTree, hTarget);
    TocBookmarkDropMode mode = GetDropModeFromTreePoint(hwndTree, hTarget, ptTree);

    if (!target) {
        if (IsPointBelowLastVisibleTreeItem(hwndTree, ptTree)) {
            mode = TocBookmarkDropMode::RootEnd;
        } else {
            mode = TocBookmarkDropMode::None;
        }
    }

    if (target == gTocBookmarkDrag.source) {
        mode = TocBookmarkDropMode::None;
    }

    if (target && IsTocItemAncestorOf(gTocBookmarkDrag.source, target)) {
        mode = TocBookmarkDropMode::None;
    }

    gTocBookmarkDrag.target = target;
    gTocBookmarkDrag.targetHItem = hTarget;
    gTocBookmarkDrag.mode = mode;

    TreeView_SelectDropTarget(hwndTree, mode == TocBookmarkDropMode::None ? nullptr : hTarget);

    logf("TOC drag update: target='%s' mode=%s ptTree=(%d,%d) hitFlags=0x%x\n",
         target && target->title ? target->title : "", TocBookmarkDropModeName(mode), ptTree.x, ptTree.y,
         (unsigned)hit.flags);
}

static void FinishTocBookmarkDrag(MainWindow* win) {
    HWND hwndTree = win && win->tocTreeView ? win->tocTreeView->hwnd : nullptr;

    // Recompute target at the final cursor position if your current version has
    // UpdateTocBookmarkDrag(win). If your current signature still takes hwnd/lp,
    // skip this line and use the last tracked target.
    // UpdateTocBookmarkDrag(win);

    TocItem* source = gTocBookmarkDrag.source;
    TocItem* target = gTocBookmarkDrag.target;
    TocBookmarkDropMode mode = gTocBookmarkDrag.mode;

    logf("TOC drag end: source='%s' target='%s' mode=%s\n", source && source->title ? source->title : "",
         target && target->title ? target->title : "", TocBookmarkDropModeName(mode));

    ResetTocBookmarkDrag(hwndTree);

    if (GetCapture()) {
        ReleaseCapture();
    }

    if (!source || mode == TocBookmarkDropMode::None) {
        return;
    }

    if (!target && mode != TocBookmarkDropMode::RootEnd) {
        return;
    }

    MovePdfBookmarkByDrop(win, source, target, mode);
}

static void CancelTocBookmarkDrag(MainWindow* win) {
    HWND hwndTree = win && win->tocTreeView ? win->tocTreeView->hwnd : nullptr;

    if (gTocBookmarkDrag.active) {
        logf("TOC drag cancel\n");
    }

    ResetTocBookmarkDrag(hwndTree);
}

static LRESULT CALLBACK WndProcTocBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId, DWORD_PTR data) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    TreeView* treeView = win->tocTreeView;

    if (msg == WM_NOTIFY) {
        NMHDR* hdr = (NMHDR*)lp;

        if (hdr && treeView && treeView->hwnd && hdr->hwndFrom == treeView->hwnd) {
            switch (hdr->code) {
                case TVN_ENDLABELEDITW: {
                    auto info = (NMTVDISPINFOW*)lp;
                    return CommitRenameTocBookmark(win, info) ? TRUE : FALSE;
                }

                case TVN_BEGINDRAGW: {
                    auto ntv = (NMTREEVIEWW*)lp;
                    return BeginTocBookmarkDrag(win, hwnd, ntv) ? TRUE : FALSE;
                }
            }
        }
    }

    if (gTocBookmarkDrag.active) {
        switch (msg) {
            case WM_MOUSEMOVE:
                UpdateTocBookmarkDrag(win);
                return 0;

            case WM_LBUTTONUP:
                FinishTocBookmarkDrag(win);
                return 0;

            case WM_RBUTTONUP:
            case WM_CANCELMODE:
                CancelTocBookmarkDrag(win);
                return 0;

            case WM_CAPTURECHANGED:
                if ((HWND)lp != hwnd) {
                    CancelTocBookmarkDrag(win);
                }
                return 0;

            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) {
                    CancelTocBookmarkDrag(win);
                    return 0;
                }
                break;
        }
    }

    LRESULT res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }

    switch (msg) {
        case WM_SIZE:
            LayoutTocContainer(win);
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_TOC_LABEL_WITH_CLOSE) {
                ToggleTocBox(win);
            }
            break;
    }

    return DefSubclassProc(hwnd, msg, wp, lp);
}


static void SubclassToc(MainWindow* win) {
    HWND hwndTocBox = win->hwndTocBox;

    if (win->tocBoxSubclassId == 0) {
        win->tocBoxSubclassId = NextSubclassId();
        BOOL ok = SetWindowSubclass(hwndTocBox, WndProcTocBox, win->tocBoxSubclassId, (DWORD_PTR)win);
        ReportIf(!ok);
    }
}

void UnsubclassToc(MainWindow* win) {
    if (win->tocBoxSubclassId != 0) {
        RemoveWindowSubclass(win->hwndTocBox, WndProcTocBox, win->tocBoxSubclassId);
        win->tocBoxSubclassId = 0;
    }
}

// TODO: restore
#if 0
void TocTreeMouseWheelHandler(MouseWheelEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->hwnd);
    ReportIf(!win);
    if (!win) {
        return;
    }
    // scroll the canvas if the cursor isn't over the ToC tree
    if (!IsCursorOverWindow(ev->hwnd)) {
        ev->didHandle = true;
        ev->result = SendMessageW(win->hwndCanvas, ev->msg, ev->wp, ev->lp);
    }
}
#endif

// TODO: restore
#if 0
void TocTreeCharHandler(CharEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->hwnd);
    ReportIf(!win);
    if (!win) {
        return;
    }
    if (VK_ESCAPE != ev->keyCode) {
        return;
    }
    if (!gGlobalPrefs->escToExit) {
        return;
    }
    if (!CanCloseWindow(win)) {
        return;
    }

    CloseWindow(win, true, false);
    ev->didHandle = true;
}
#endif

// Recursively build a filtered copy of the TocItem tree.
// Includes items whose title matches the filter, plus ancestors needed to reach them.
// Returns nullptr if nothing matches.
static TocItem* FilterTocItemRec(TocItem* item, const char* filter) {
    if (!item) {
        return nullptr;
    }
    TocItem* resultFirst = nullptr;
    TocItem* resultLast = nullptr;
    for (TocItem* si = item; si; si = si->next) {
        // recursively filter children
        TocItem* filteredChildren = FilterTocItemRec(si->child, filter);
        bool titleMatches = si->title && str::ContainsI(si->title, filter);
        if (!titleMatches && !filteredChildren) {
            continue;
        }
        // create a copy of this item
        auto* copy = new TocItem();
        copy->title = str::Dup(si->title);
        copy->pageNo = si->pageNo;
        copy->id = si->id;
        copy->fontFlags = si->fontFlags;
        copy->color = si->color;
        copy->dest = si->dest;
        copy->destNotOwned = true;
        copy->isOpenDefault = true;
        copy->isOpenToggled = false;
        copy->child = filteredChildren;
        // set parent pointers on children
        for (TocItem* c = copy->child; c; c = c->next) {
            c->parent = copy;
        }
        if (!resultFirst) {
            resultFirst = copy;
            resultLast = copy;
        } else {
            resultLast->next = copy;
            resultLast = copy;
        }
    }
    return resultFirst;
}

static void ApplyTocFilter(MainWindow* win, const char* filter) {
    if (!win->tocLoaded) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->currToc) {
        return;
    }
    // free previous filtered tree
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;

    TreeView* treeView = win->tocTreeView;
    TocTree* origTree = tab->currToc;

    if (!filter || str::Len(filter) == 0) {
        // restore original tree
        SetInitialExpandState(origTree->root, tab->tocState);
        treeView->SetTreeModel(origTree);
        return;
    }

    TocItem* filteredRoot = FilterTocItemRec(origTree->root, filter);
    if (!filteredRoot) {
        treeView->Clear();
        return;
    }
    auto* filteredTree = new TocTree(filteredRoot);
    win->tocFilteredTree = filteredTree;
    treeView->SetTreeModel(filteredTree);
}

void TocFilterChanged(MainWindow* win) {
    Edit* edit = win->tocFilterEdit;
    if (!edit) {
        return;
    }
    TempStr filter = edit->GetTextTemp();
    ApplyTocFilter(win, filter);
}

static void OnTocFilterTextChanged(MainWindow* win) {
    TocFilterChanged(win);
}

static LRESULT CALLBACK WndProcTocFilterEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId,
                                             DWORD_PTR data) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        MainWindow* win = (MainWindow*)data;
        Edit* edit = win->tocFilterEdit;
        if (edit) {
            TempStr txt = edit->GetTextTemp();
            if (txt && str::Len(txt) > 0) {
                edit->SetText("");
                // onTextChanged will fire and restore the tree
                return 0;
            }
            // if already empty, move focus to tree
            SetFocus(win->tocTreeView->hwnd);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void CreateToc(MainWindow* win) {
    HMODULE hmod = GetModuleHandle(nullptr);
    int dx = gGlobalPrefs->sidebarDx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN;
    HWND parent = win->hwndFrame;
    win->hwndTocBox = CreateWindowExW(0, WC_STATIC, L"", style, 0, 0, dx, 0, parent, nullptr, hmod, nullptr);

    auto l = new LabelWithCloseWnd();
    {
        LabelWithCloseWnd::CreateArgs args;
        args.parent = win->hwndTocBox;
        args.cmdId = IDC_TOC_LABEL_WITH_CLOSE;
        args.isRtl = IsUIRtl();
        // TODO: use the same font size as in GetTreeFont()?
        args.font = GetDefaultGuiFont(true, false);
        l->Create(args);
    }
    win->tocLabelWithClose = l;
    l->SetPaddingXY(2, 2);
    // label is set in UpdateToolbarSidebarText()

    auto filterEdit = new Edit();
    {
        Edit::CreateArgs eargs;
        eargs.parent = win->hwndTocBox;
        eargs.withBorder = false;
        eargs.cueText = _TRA("Search Bookmarks");
        eargs.font = GetDefaultGuiFont(false, false);
        filterEdit->Create(eargs);
    }
    win->tocFilterEdit = filterEdit;
    filterEdit->onTextChanged = MkFunc0(OnTocFilterTextChanged, win);
    SetWindowSubclass(filterEdit->hwnd, WndProcTocFilterEdit, NextSubclassId(), (DWORD_PTR)win);

    auto treeView = new TreeView();
    TreeView::CreateArgs args;
    args.parent = win->hwndTocBox;
    args.font = GetAppTreeFont();
    args.fullRowSelect = true;
    args.exStyle = 0;
    args.isRtl = IsUIRtl();

    auto fn = MkFunc1Void(TocContextMenu);
    treeView->onContextMenu = fn;
    treeView->onSelectionChanged = MkFunc1Void(TocTreeSelectionChanged);
    treeView->onKeyDown = MkFunc1Void(TocTreeKeyDown2);
    treeView->onGetTooltip = MkFunc1Void(TocCustomizeTooltip);
    // treeView->onClick = TocTreeClick; // TODO: maybe not necessary
    // treeView->onChar = TocTreeCharHandler;
    // treeView->onMouseWheel = TocTreeMouseWheelHandler;

    treeView->Create(args);
    ReportIf(!treeView->hwnd);
    win->tocTreeView = treeView;

    SubclassToc(win);

    UpdateControlsColors(win);
}
