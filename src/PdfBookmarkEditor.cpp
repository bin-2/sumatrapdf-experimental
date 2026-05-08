extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/GdiPlusUtil.h"
#include "wingui/UIModels.h"

#include "Annotation.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineMupdf.h"
#include "PdfBookmarkEditor.h"

// ================================================================
// Static helper declarations
// ================================================================

// Path helpers
static void CopyPath(const Vec<int>& src, Vec<int>& dst);
static bool BuildSiblingPath(const Vec<int>& path, int delta, Vec<int>& siblingPath);
static bool PathEquals(const Vec<int>& a, const Vec<int>& b);
static bool PathIsPrefixOf(const Vec<int>& prefix, const Vec<int>& path);
static void CopyPathWithIncrementAt(const Vec<int>& src, int incrementIndex, Vec<int>& dst);

// MuPDF outline cloning and iterator helpers
static fz_outline* CloneOutlineList(fz_context* ctx, fz_outline* src);
static fz_outline* CloneOutlineNodeTree(fz_context* ctx, fz_outline* src);
static bool MoveOutlineIteratorToPath(fz_context* ctx, fz_outline_iterator* iter, const Vec<int>& path);
static bool MoveOutlineIteratorAfterPath(fz_context* ctx, fz_outline_iterator* iter, const Vec<int>& path);
static bool MoveOutlineIteratorBelowPath(fz_context* ctx, fz_outline_iterator* iter, const Vec<int>& path);
static bool MoveOutlineIteratorToRootEnd(fz_context* ctx, fz_outline_iterator* iter);
static void InsertOutlineSnapshotAtCurrent(fz_context* ctx, fz_outline_iterator* iter, fz_outline* node);

// Persistent PDF bookmark mutations
static bool RenameBookmarkInPdf(EngineMupdf* epdf, const Vec<int>& path, const char* oldTitle, const char* newTitle);
static bool AddRootBookmarkInPdf(EngineMupdf* epdf, const char* title, int pageNo, char** uriOut);
static bool AddBookmarkAfterInPdf(EngineMupdf* epdf, const Vec<int>& afterPath, const char* title, int pageNo,
                                  char** uriOut);
static bool AddChildBookmarkInPdf(EngineMupdf* epdf, const Vec<int>& parentPath, const char* title, int pageNo,
                                  char** uriOut);
static bool DeleteBookmarkInPdf(EngineMupdf* epdf, const Vec<int>& path);
static bool DeleteBookmarkInPdfUnlocked(fz_context* ctx, EngineMupdf* epdf, const Vec<int>& path);
static bool MoveBookmarkUpDownInPdf(EngineMupdf* epdf, const Vec<int>& path, bool moveDown, fz_outline* snapshot);
static bool MoveBookmarkAsChildInPdf(EngineMupdf* epdf, const Vec<int>& sourcePath, const Vec<int>& targetParentPath,
                                     fz_outline* snapshot);
static bool MoveBookmarkToRootEndInPdf(EngineMupdf* epdf, const Vec<int>& sourcePath, fz_outline* snapshot);
static void MarkPdfBookmarksModified(EngineMupdf* epdf);

// Session outline mutations
static fz_outline* CreateSessionOutlineNode(fz_context* ctx, fz_document* doc, const char* title, const char* uri,
                                            int pageNo);
static fz_outline* InsertSessionOutlineAtRootEnd(fz_context* ctx, fz_document* doc, fz_outline*& root,
                                                 const char* title, const char* uri, int pageNo);
static fz_outline* InsertSessionOutlineAfterPath(fz_context* ctx, fz_document* doc, fz_outline*& root,
                                                 const Vec<int>& afterPath, const char* title, const char* uri,
                                                 int pageNo);
static fz_outline* InsertSessionOutlineChildAtPath(fz_context* ctx, fz_document* doc, fz_outline*& root,
                                                   const Vec<int>& parentPath, const char* title, const char* uri,
                                                   int pageNo);
static bool RemoveSessionOutlineAtPath(fz_context* ctx, fz_outline*& root, const Vec<int>& path);
static fz_outline** GetOutlineSiblingListForPath(fz_outline*& root, const Vec<int>& path);
static fz_outline** GetOutlineLinkAtPath(fz_outline*& root, const Vec<int>& path);
static bool MoveSessionOutlineUpDownAtPath(fz_outline*& root, const Vec<int>& path, bool moveDown);
static bool MoveSessionOutlineAsChildAtPath(fz_outline*& root, const Vec<int>& sourcePath,
                                            const Vec<int>& targetParentPath);
static bool MoveSessionOutlineToRootEndAtPath(fz_outline*& root, const Vec<int>& sourcePath);

// TOC tree mutations
static TocItem* AddRootTocItemInMemory(TocTree* tocTree, const char* title, int pageNo);
static TocItem* AddTocItemAfterInMemory(TocItem* afterItem, const char* title, int pageNo);
static TocItem* AddChildTocItemInMemory(TocItem* parentItem, const char* title, int pageNo);
static bool RemoveTocItemAtPath(TocTree* tocTree, const Vec<int>& path);
static TocItem** GetTocSiblingListForPath(TocTree* tocTree, const Vec<int>& path);
static TocItem** GetTocItemLinkAtPath(TocTree* tocTree, const Vec<int>& path);
static void SetTocItemParentRecursive(TocItem* item, TocItem* parent);
static bool MoveTocItemUpDownAtPath(TocTree* tocTree, const Vec<int>& path, bool moveDown);
static bool MoveTocItemAsChildAtPath(TocTree* tocTree, const Vec<int>& sourcePath, const Vec<int>& targetParentPath);
static bool MoveTocItemToRootEndAtPath(TocTree* tocTree, const Vec<int>& sourcePath);

// ================================================================
// PdfBookmarkEditor construction and capability checks
// ================================================================

PdfBookmarkEditor::PdfBookmarkEditor(EngineBase* engine) {
    this->engine = engine;
}

bool PdfBookmarkEditor::CanEditBookmarks() const {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    return epdf && epdf->pdfdoc;
}

bool PdfBookmarkEditor::CanRenameBookmarks() const {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    return epdf && epdf->pdfdoc && epdf->outline;
}

bool PdfBookmarkEditor::CanAddBookmarks() const {
    return CanEditBookmarks();
}

bool PdfBookmarkEditor::CanDeleteBookmarks() const {
    return CanRenameBookmarks();
}

bool PdfBookmarkEditor::CanMoveBookmarks() const {
    return CanRenameBookmarks();
}

bool PdfBookmarkEditor::CanMoveBookmarkUp(TocItem* item) const {
    if (!CanMoveBookmarks() || !item) {
        return false;
    }

    Vec<int> path;
    if (!GetTocItemPath(item, path) || path.Size() == 0) {
        return false;
    }

    int index = path.At(path.Size() - 1);
    return index > 0;
}

bool PdfBookmarkEditor::CanMoveBookmarkDown(TocItem* item) const {
    if (!CanMoveBookmarks() || !item) {
        return false;
    }

    return item->next != nullptr;
}

bool PdfBookmarkEditor::CanMoveBookmarkAsChild(TocItem* sourceItem, TocItem* targetParentItem) const {
    if (!CanMoveBookmarks() || !sourceItem || !targetParentItem) {
        return false;
    }

    if (sourceItem == targetParentItem) {
        return false;
    }

    Vec<int> sourcePath;
    Vec<int> targetParentPath;

    if (!GetTocItemPath(sourceItem, sourcePath) || !GetTocItemPath(targetParentItem, targetParentPath)) {
        return false;
    }

    if (PathEquals(sourcePath, targetParentPath)) {
        return false;
    }

    // Cannot move an item into itself or any descendant.
    if (PathIsPrefixOf(sourcePath, targetParentPath)) {
        return false;
    }

    return true;
}

bool PdfBookmarkEditor::CanMoveBookmarkToRootEnd(TocItem* sourceItem) const {
    if (!CanMoveBookmarks() || !sourceItem) {
        return false;
    }

    Vec<int> sourcePath;
    if (!GetTocItemPath(sourceItem, sourcePath) || sourcePath.Size() == 0) {
        return false;
    }

    // Dropping the last root item below the tree is a no-op.
    if (sourcePath.Size() == 1 && !sourceItem->next) {
        return false;
    }

    return true;
}

// ================================================================
// PdfBookmarkEditor add operations
// ================================================================

bool PdfBookmarkEditor::AddRootBookmark(const char* title, int pageNo) {
    if (!CanEditBookmarks()) {
        return false;
    }

    if (!title || title[0] == '\0' || pageNo <= 0) {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);
    TocTree* tocTree = GetOrCreateTocTree();
    if (!epdf || !tocTree || !tocTree->root) {
        return false;
    }

    char* uri = nullptr;
    bool pdfUpdated = AddRootBookmarkInPdf(epdf, title, pageNo, &uri);
    if (!pdfUpdated || !uri) {
        ReportIf(true);
        return false;
    }

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        fz_outline* newOutline = InsertSessionOutlineAtRootEnd(ctx, epdf->_doc, epdf->outline, title, uri, pageNo);
        ReportIf(!newOutline);

        LeaveCriticalSection(&epdf->docLock);

        fz_free(ctx, uri);
        uri = nullptr;
    }

    TocItem* newTocItem = AddRootTocItemInMemory(tocTree, title, pageNo);
    if (!newTocItem) {
        ReportIf(true);
        return false;
    }

    MarkPdfBookmarksModified(epdf);
    return true;
}

bool PdfBookmarkEditor::AddBookmarkAfter(TocItem* afterItem, const char* title, int pageNo) {
    if (!CanEditBookmarks()) {
        return false;
    }

    if (!afterItem || !title || title[0] == '\0' || pageNo <= 0) {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);

    Vec<int> afterPath;
    if (!GetTocItemPath(afterItem, afterPath)) {
        ReportIf(true);
        return false;
    }

    char* uri = nullptr;
    bool pdfUpdated = AddBookmarkAfterInPdf(epdf, afterPath, title, pageNo, &uri);
    if (!pdfUpdated || !uri) {
        ReportIf(true);
        return false;
    }

    TocItem* newTocItem = nullptr;

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        fz_outline* newOutline =
            InsertSessionOutlineAfterPath(ctx, epdf->_doc, epdf->outline, afterPath, title, uri, pageNo);
        ReportIf(!newOutline);

        LeaveCriticalSection(&epdf->docLock);

        fz_free(ctx, uri);
        uri = nullptr;
    }

    newTocItem = AddTocItemAfterInMemory(afterItem, title, pageNo);
    if (!newTocItem) {
        ReportIf(true);
        return false;
    }

    MarkPdfBookmarksModified(epdf);
    return true;
}

bool PdfBookmarkEditor::AddChildBookmark(TocItem* parentItem, const char* title, int pageNo) {
    if (!CanEditBookmarks()) {
        return false;
    }

    if (!parentItem || !title || title[0] == '\0' || pageNo <= 0) {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);

    Vec<int> parentPath;
    if (!GetTocItemPath(parentItem, parentPath)) {
        ReportIf(true);
        return false;
    }

    char* uri = nullptr;
    bool pdfUpdated = AddChildBookmarkInPdf(epdf, parentPath, title, pageNo, &uri);
    if (!pdfUpdated || !uri) {
        ReportIf(true);
        return false;
    }

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        fz_outline* newOutline =
            InsertSessionOutlineChildAtPath(ctx, epdf->_doc, epdf->outline, parentPath, title, uri, pageNo);
        ReportIf(!newOutline);

        LeaveCriticalSection(&epdf->docLock);

        fz_free(ctx, uri);
        uri = nullptr;
    }

    TocItem* newTocItem = AddChildTocItemInMemory(parentItem, title, pageNo);
    if (!newTocItem) {
        ReportIf(true);
        return false;
    }

    // The selected item now has a child. Make that visible by default.
    parentItem->isOpenDefault = true;
    parentItem->isOpenToggled = true;

    MarkPdfBookmarksModified(epdf);
    return true;
}

// ================================================================
// PdfBookmarkEditor rename and delete operations
// ================================================================

bool PdfBookmarkEditor::RenameBookmark(TocItem* item, const char* newTitle) {
    if (!item || !newTitle || newTitle[0] == '\0') {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);

    Vec<int> path;
    bool hasPath = false;
    bool pdfUpdated = false;

    if (epdf && epdf->pdfdoc) {
        hasPath = GetTocItemPath(item, path);

        if (hasPath) {
            const char* oldTitle = item->title ? item->title : "";
            pdfUpdated = RenameBookmarkInPdf(epdf, path, oldTitle, newTitle);
        }
    }

    str::ReplaceWithCopy(&item->title, newTitle);

    if (!epdf || !epdf->pdfdoc) {
        ReportIf(true);
        return true;
    }

    if (!hasPath) {
        ReportIf(true);
        return true;
    }

    if (!pdfUpdated) {
        ReportIf(true);
        return true;
    }

    // Keep loaded outline memory in sync for the current session.
    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        fz_outline* outlineItem = FindOutlineByPath(path);
        if (outlineItem) {
            char* titleCopy = fz_strdup(ctx, newTitle);
            if (titleCopy) {
                fz_free(ctx, outlineItem->title);
                outlineItem->title = titleCopy;
            }
        }

        LeaveCriticalSection(&epdf->docLock);
    }

    MarkPdfBookmarksModified(epdf);
    return true;
}

bool PdfBookmarkEditor::DeleteBookmark(TocItem* item) {
    if (!CanDeleteBookmarks()) {
        return false;
    }

    if (!item) {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);

    Vec<int> path;
    if (!GetTocItemPath(item, path)) {
        ReportIf(true);
        return false;
    }

    bool pdfUpdated = DeleteBookmarkInPdf(epdf, path);
    if (!pdfUpdated) {
        ReportIf(true);
        return false;
    }

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        bool outlineRemoved = RemoveSessionOutlineAtPath(ctx, epdf->outline, path);
        ReportIf(!outlineRemoved);

        LeaveCriticalSection(&epdf->docLock);
    }

    bool tocRemoved = RemoveTocItemAtPath(epdf->tocTree, path);
    if (!tocRemoved) {
        ReportIf(true);
        return false;
    }

    MarkPdfBookmarksModified(epdf);
    return true;
}

// ================================================================
// PdfBookmarkEditor move operations
// ================================================================

bool PdfBookmarkEditor::MoveBookmarkUp(TocItem* item) {
    return MoveBookmarkUpDown(item, false);
}

bool PdfBookmarkEditor::MoveBookmarkDown(TocItem* item) {
    return MoveBookmarkUpDown(item, true);
}

bool PdfBookmarkEditor::MoveBookmarkUpDown(TocItem* item, bool moveDown) {
    if (!CanMoveBookmarks() || !item) {
        return false;
    }

    if (moveDown) {
        if (!CanMoveBookmarkDown(item)) {
            return false;
        }
    } else {
        if (!CanMoveBookmarkUp(item)) {
            return false;
        }
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);

    Vec<int> path;
    if (!GetTocItemPath(item, path)) {
        ReportIf(true);
        return false;
    }

    fz_outline* snapshot = nullptr;

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        fz_outline* outlineItem = FindOutlineByPath(path);
        if (outlineItem) {
            snapshot = CloneOutlineNodeTree(ctx, outlineItem);
        }

        LeaveCriticalSection(&epdf->docLock);
    }

    if (!snapshot) {
        ReportIf(true);
        return false;
    }

    bool pdfUpdated = MoveBookmarkUpDownInPdf(epdf, path, moveDown, snapshot);

    {
        fz_context* ctx = epdf->Ctx();
        fz_drop_outline(ctx, snapshot);
        snapshot = nullptr;
    }

    if (!pdfUpdated) {
        ReportIf(true);
        return false;
    }

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        bool outlineMoved = MoveSessionOutlineUpDownAtPath(epdf->outline, path, moveDown);
        ReportIf(!outlineMoved);

        LeaveCriticalSection(&epdf->docLock);
    }

    bool tocMoved = MoveTocItemUpDownAtPath(epdf->tocTree, path, moveDown);
    if (!tocMoved) {
        ReportIf(true);
        return false;
    }

    MarkPdfBookmarksModified(epdf);
    return true;
}

bool PdfBookmarkEditor::MoveBookmarkAsChild(TocItem* sourceItem, TocItem* targetParentItem) {
    if (!CanMoveBookmarkAsChild(sourceItem, targetParentItem)) {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->pdfdoc || !epdf->tocTree) {
        return false;
    }

    Vec<int> sourcePath;
    Vec<int> targetParentPath;

    if (!GetTocItemPath(sourceItem, sourcePath) || !GetTocItemPath(targetParentItem, targetParentPath)) {
        ReportIf(true);
        return false;
    }

    fz_outline* snapshot = nullptr;

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        fz_outline* outlineItem = FindOutlineByPath(sourcePath);
        if (outlineItem) {
            snapshot = CloneOutlineNodeTree(ctx, outlineItem);
        }

        LeaveCriticalSection(&epdf->docLock);
    }

    if (!snapshot) {
        ReportIf(true);
        return false;
    }

    bool pdfUpdated = MoveBookmarkAsChildInPdf(epdf, sourcePath, targetParentPath, snapshot);

    {
        fz_context* ctx = epdf->Ctx();
        fz_drop_outline(ctx, snapshot);
        snapshot = nullptr;
    }

    if (!pdfUpdated) {
        ReportIf(true);
        return false;
    }

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        bool outlineMoved = MoveSessionOutlineAsChildAtPath(epdf->outline, sourcePath, targetParentPath);
        ReportIf(!outlineMoved);

        LeaveCriticalSection(&epdf->docLock);
    }

    bool tocMoved = MoveTocItemAsChildAtPath(epdf->tocTree, sourcePath, targetParentPath);
    if (!tocMoved) {
        ReportIf(true);
        return false;
    }

    MarkPdfBookmarksModified(epdf);
    return true;
}

bool PdfBookmarkEditor::MoveBookmarkToRootEnd(TocItem* sourceItem) {
    if (!CanMoveBookmarkToRootEnd(sourceItem)) {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->pdfdoc || !epdf->tocTree) {
        return false;
    }

    Vec<int> sourcePath;
    if (!GetTocItemPath(sourceItem, sourcePath)) {
        ReportIf(true);
        return false;
    }

    fz_outline* snapshot = nullptr;

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        fz_outline* outlineItem = FindOutlineByPath(sourcePath);
        if (outlineItem) {
            snapshot = CloneOutlineNodeTree(ctx, outlineItem);
        }

        LeaveCriticalSection(&epdf->docLock);
    }

    if (!snapshot) {
        ReportIf(true);
        return false;
    }

    bool pdfUpdated = MoveBookmarkToRootEndInPdf(epdf, sourcePath, snapshot);

    {
        fz_context* ctx = epdf->Ctx();
        fz_drop_outline(ctx, snapshot);
        snapshot = nullptr;
    }

    if (!pdfUpdated) {
        ReportIf(true);
        return false;
    }

    {
        fz_context* ctx = epdf->Ctx();

        EnterCriticalSection(&epdf->docLock);

        bool outlineMoved = MoveSessionOutlineToRootEndAtPath(epdf->outline, sourcePath);
        ReportIf(!outlineMoved);

        LeaveCriticalSection(&epdf->docLock);
    }

    bool tocMoved = MoveTocItemToRootEndAtPath(epdf->tocTree, sourcePath);
    if (!tocMoved) {
        ReportIf(true);
        return false;
    }

    MarkPdfBookmarksModified(epdf);
    return true;
}

// ================================================================
// PdfBookmarkEditor tree and path lookup helpers
// ================================================================

TocTree* PdfBookmarkEditor::GetOrCreateTocTree() {
    if (!CanEditBookmarks()) {
        return nullptr;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return nullptr;
    }

    if (epdf->tocTree && epdf->tocTree->root) {
        return epdf->tocTree;
    }

    auto* root = new TocItem();
    epdf->tocTree = new TocTree(root);

    return epdf->tocTree;
}

bool PdfBookmarkEditor::FindTocItemPathRecursive(TocItem* root, TocItem* wanted, Vec<int>& pathOut) const {
    int index = 0;

    for (TocItem* item = root; item; item = item->next) {
        pathOut.Append(index);

        if (item == wanted) {
            return true;
        }

        if (item->child && FindTocItemPathRecursive(item->child, wanted, pathOut)) {
            return true;
        }

        pathOut.Pop();
        index++;
    }

    return false;
}

bool PdfBookmarkEditor::GetTocItemPath(TocItem* item, Vec<int>& pathOut) const {
    pathOut.Reset();

    if (!item) {
        return false;
    }

    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->tocTree || !epdf->tocTree->root) {
        return false;
    }

    TocItem* firstRealItem = epdf->tocTree->root->child;
    if (!firstRealItem) {
        return false;
    }

    return FindTocItemPathRecursive(firstRealItem, item, pathOut);
}

fz_outline* PdfBookmarkEditor::FindOutlineByPath(const Vec<int>& path) const {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->outline) {
        return nullptr;
    }

    fz_outline* node = epdf->outline;

    for (int depth = 0; depth < path.Size(); depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return nullptr;
        }

        while (node && index > 0) {
            node = node->next;
            index--;
        }

        if (!node) {
            return nullptr;
        }

        if (depth + 1 < path.Size()) {
            node = node->down;
        }
    }

    return node;
}

// ================================================================
// Path helpers
// ================================================================

static void CopyPath(const Vec<int>& src, Vec<int>& dst) {
    dst.Reset();

    for (int i = 0; i < src.Size(); i++) {
        dst.Append(src.At(i));
    }
}

static bool BuildSiblingPath(const Vec<int>& path, int delta, Vec<int>& siblingPath) {
    siblingPath.Reset();

    if (path.Size() == 0) {
        return false;
    }

    int lastIndex = path.At(path.Size() - 1) + delta;
    if (lastIndex < 0) {
        return false;
    }

    for (int i = 0; i < path.Size() - 1; i++) {
        siblingPath.Append(path.At(i));
    }

    siblingPath.Append(lastIndex);
    return true;
}

static bool PathEquals(const Vec<int>& a, const Vec<int>& b) {
    if (a.Size() != b.Size()) {
        return false;
    }

    for (int i = 0; i < a.Size(); i++) {
        if (a.At(i) != b.At(i)) {
            return false;
        }
    }

    return true;
}

static bool PathIsPrefixOf(const Vec<int>& prefix, const Vec<int>& path) {
    if (prefix.Size() > path.Size()) {
        return false;
    }

    for (int i = 0; i < prefix.Size(); i++) {
        if (prefix.At(i) != path.At(i)) {
            return false;
        }
    }

    return true;
}

static void CopyPathWithIncrementAt(const Vec<int>& src, int incrementIndex, Vec<int>& dst) {
    dst.Reset();

    for (int i = 0; i < src.Size(); i++) {
        int value = src.At(i);

        if (i == incrementIndex) {
            value++;
        }

        dst.Append(value);
    }
}

// ================================================================
// MuPDF outline cloning and iterator helpers
// ================================================================

static fz_outline* CloneOutlineList(fz_context* ctx, fz_outline* src) {
    fz_outline* first = nullptr;
    fz_outline** tail = &first;

    for (fz_outline* n = src; n; n = n->next) {
        fz_outline* copy = CloneOutlineNodeTree(ctx, n);
        if (!copy) {
            fz_drop_outline(ctx, first);
            return nullptr;
        }

        *tail = copy;
        tail = &copy->next;
    }

    return first;
}

static fz_outline* CloneOutlineNodeTree(fz_context* ctx, fz_outline* src) {
    if (!ctx || !src) {
        return nullptr;
    }

    fz_outline* dst = fz_new_outline(ctx);
    if (!dst) {
        return nullptr;
    }

    fz_try(ctx) {
        dst->title = src->title ? fz_strdup(ctx, src->title) : nullptr;
        dst->uri = src->uri ? fz_strdup(ctx, src->uri) : nullptr;

        dst->page = src->page;
        dst->x = src->x;
        dst->y = src->y;

        dst->is_open = src->is_open;
        dst->r = src->r;
        dst->g = src->g;
        dst->b = src->b;
        dst->flags = src->flags;

        dst->down = CloneOutlineList(ctx, src->down);
    }
    fz_catch(ctx) {
        fz_drop_outline(ctx, dst);
        return nullptr;
    }

    return dst;
}

static bool MoveOutlineIteratorToPath(fz_context* ctx, fz_outline_iterator* iter, const Vec<int>& path) {
    if (!ctx || !iter || path.Size() == 0) {
        return false;
    }

    // Important:
    // fz_new_outline_iterator() already points at the first root outline item.
    // Do not call fz_outline_iterator_next() here, or path[0] will incorrectly
    // point to the second root item.
    if (!fz_outline_iterator_item(ctx, iter)) {
        return false;
    }

    for (int depth = 0; depth < path.Size(); depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return false;
        }

        for (int i = 0; i < index; i++) {
            int rc = fz_outline_iterator_next(ctx, iter);
            if (rc != 0) {
                return false;
            }
        }

        if (depth + 1 < path.Size()) {
            int rc = fz_outline_iterator_down(ctx, iter);
            if (rc != 0) {
                return false;
            }
        }
    }

    return true;
}

static bool MoveOutlineIteratorAfterPath(fz_context* ctx, fz_outline_iterator* iter, const Vec<int>& path) {
    if (!MoveOutlineIteratorToPath(ctx, iter, path)) {
        return false;
    }

    int rc = fz_outline_iterator_next(ctx, iter);

    // rc == 0: now positioned at next sibling, insert before it.
    // rc == 1: now positioned at empty end position, insert there.
    // rc < 0: failed.
    return rc == 0 || rc == 1;
}

static bool MoveOutlineIteratorBelowPath(fz_context* ctx, fz_outline_iterator* iter, const Vec<int>& path) {
    if (!MoveOutlineIteratorToPath(ctx, iter, path)) {
        return false;
    }

    int rc = fz_outline_iterator_down(ctx, iter);

    // rc == 0: now positioned at first child, insert before it.
    // rc == 1: now positioned below parent, empty child list, insert first child.
    // rc < 0: failed.
    return rc == 0 || rc == 1;
}

static bool MoveOutlineIteratorToRootEnd(fz_context* ctx, fz_outline_iterator* iter) {
    if (!ctx || !iter) {
        return false;
    }

    if (!fz_outline_iterator_item(ctx, iter)) {
        // Empty outline. This should not normally happen when moving an existing
        // bookmark, but it is still an insertable position.
        return true;
    }

    for (;;) {
        int rc = fz_outline_iterator_next(ctx, iter);
        if (rc == 1) {
            return true;
        }

        if (rc < 0) {
            return false;
        }
    }
}

static void InsertOutlineSnapshotAtCurrent(fz_context* ctx, fz_outline_iterator* iter, fz_outline* node) {
    if (!ctx || !iter || !node) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid outline snapshot insertion");
    }

    fz_outline_item item = {};
    item.title = node->title;
    item.uri = node->uri;
    item.is_open = node->is_open;
    item.r = node->r;
    item.g = node->g;
    item.b = node->b;
    item.flags = node->flags;

    fz_outline_iterator_insert(ctx, iter, &item);

    if (!node->down) {
        return;
    }

    int rc = fz_outline_iterator_down(ctx, iter);
    if (rc != 0 && rc != 1) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "failed to enter inserted outline child list");
    }

    for (fz_outline* child = node->down; child; child = child->next) {
        InsertOutlineSnapshotAtCurrent(ctx, iter, child);

        if (child->next) {
            rc = fz_outline_iterator_next(ctx, iter);
            if (rc != 0 && rc != 1) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "failed to advance inserted outline child list");
            }
        }
    }

    rc = fz_outline_iterator_up(ctx, iter);
    if (rc != 0) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "failed to leave inserted outline child list");
    }
}

// ================================================================
// Persistent PDF bookmark mutations
// ================================================================

static bool RenameBookmarkInPdf(EngineMupdf* epdf, const Vec<int>& path, const char* oldTitle, const char* newTitle) {
    if (!epdf || !epdf->pdfdoc || !newTitle || newTitle[0] == '\0') {
        return false;
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(ok);

    fz_try(ctx) {
        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (!MoveOutlineIteratorToPath(ctx, iter, path)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate outline item");
        }

        fz_outline_item* oldItem = fz_outline_iterator_item(ctx, iter);
        if (!oldItem) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to get outline item");
        }

        const char* pdfTitle = oldItem->title ? oldItem->title : "";
        const char* expectedTitle = oldTitle ? oldTitle : "";

        if (!str::Eq(pdfTitle, expectedTitle)) {
            ok = false;

            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        } else {
            fz_outline_item newItem = *oldItem;
            newItem.title = (char*)newTitle;

            fz_outline_iterator_update(ctx, iter, &newItem);

            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;

            ok = true;
        }
    }
    fz_catch(ctx) {
        const char* msg = fz_caught_message(ctx);

        fz_report_error(ctx);
        ok = false;

        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static bool AddRootBookmarkInPdf(EngineMupdf* epdf, const char* title, int pageNo, char** uriOut) {
    if (uriOut) {
        *uriOut = nullptr;
    }

    if (!epdf || !epdf->pdfdoc || !title || title[0] == '\0' || pageNo <= 0) {
        return false;
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    char* uri = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(uri);
    fz_var(ok);

    fz_try(ctx) {
        int pageCount = fz_count_pages(ctx, epdf->_doc);
        if (pageNo > pageCount) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid bookmark page");
        }

        fz_location loc = fz_location_from_page_number(ctx, epdf->_doc, pageNo - 1);

        fz_link_dest dest = {};
        dest.loc = loc;
        dest.type = FZ_LINK_DEST_XYZ;
        dest.x = 0;
        dest.y = 0;
        dest.zoom = 0;

        uri = fz_format_link_uri(ctx, epdf->_doc, dest);
        if (!uri) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create bookmark URI");
        }

        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        // If there are existing root items, move to one-past-last root item.
        // If there are no items, the iterator is already at an insertable empty position.
        if (fz_outline_iterator_item(ctx, iter)) {
            for (;;) {
                int rc = fz_outline_iterator_next(ctx, iter);
                if (rc == 1) {
                    break;
                }
                if (rc < 0) {
                    fz_throw(ctx, FZ_ERROR_GENERIC, "failed to find root insertion point");
                }
            }
        }

        fz_outline_item item = {};
        item.title = (char*)title;
        item.uri = uri;
        item.is_open = 0;
        item.r = 0;
        item.g = 0;
        item.b = 0;
        item.flags = 0;

        fz_outline_iterator_insert(ctx, iter, &item);

        if (uriOut) {
            *uriOut = fz_strdup(ctx, uri);
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }

        fz_free(ctx, uri);
        uri = nullptr;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;

        if (uriOut && *uriOut) {
            fz_free(ctx, *uriOut);
            *uriOut = nullptr;
        }
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static bool AddBookmarkAfterInPdf(EngineMupdf* epdf, const Vec<int>& afterPath, const char* title, int pageNo,
                                  char** uriOut) {
    if (uriOut) {
        *uriOut = nullptr;
    }

    if (!epdf || !epdf->pdfdoc || afterPath.Size() == 0 || !title || title[0] == '\0' || pageNo <= 0) {
        return false;
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    char* uri = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(uri);
    fz_var(ok);

    fz_try(ctx) {
        int pageCount = fz_count_pages(ctx, epdf->_doc);
        if (pageNo > pageCount) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid bookmark page");
        }

        fz_location loc = fz_location_from_page_number(ctx, epdf->_doc, pageNo - 1);

        fz_link_dest dest = {};
        dest.loc = loc;
        dest.type = FZ_LINK_DEST_XYZ;
        dest.x = 0;
        dest.y = 0;
        dest.zoom = 0;

        uri = fz_format_link_uri(ctx, epdf->_doc, dest);
        if (!uri) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create bookmark URI");
        }

        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (!MoveOutlineIteratorAfterPath(ctx, iter, afterPath)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate bookmark insertion point");
        }

        fz_outline_item item = {};
        item.title = (char*)title;
        item.uri = uri;
        item.is_open = 0;
        item.r = 0;
        item.g = 0;
        item.b = 0;
        item.flags = 0;

        fz_outline_iterator_insert(ctx, iter, &item);

        if (uriOut) {
            *uriOut = fz_strdup(ctx, uri);
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }

        fz_free(ctx, uri);
        uri = nullptr;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;

        if (uriOut && *uriOut) {
            fz_free(ctx, *uriOut);
            *uriOut = nullptr;
        }
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static bool AddChildBookmarkInPdf(EngineMupdf* epdf, const Vec<int>& parentPath, const char* title, int pageNo,
                                  char** uriOut) {
    if (uriOut) {
        *uriOut = nullptr;
    }

    if (!epdf || !epdf->pdfdoc || parentPath.Size() == 0 || !title || title[0] == '\0' || pageNo <= 0) {
        return false;
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    char* uri = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(uri);
    fz_var(ok);

    fz_try(ctx) {
        int pageCount = fz_count_pages(ctx, epdf->_doc);
        if (pageNo > pageCount) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid bookmark page");
        }

        fz_location loc = fz_location_from_page_number(ctx, epdf->_doc, pageNo - 1);

        fz_link_dest dest = {};
        dest.loc = loc;
        dest.type = FZ_LINK_DEST_XYZ;
        dest.x = 0;
        dest.y = 0;
        dest.zoom = 0;

        uri = fz_format_link_uri(ctx, epdf->_doc, dest);
        if (!uri) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create bookmark URI");
        }

        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (!MoveOutlineIteratorBelowPath(ctx, iter, parentPath)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate bookmark child insertion point");
        }

        fz_outline_item item = {};
        item.title = (char*)title;
        item.uri = uri;
        item.is_open = 0;
        item.r = 0;
        item.g = 0;
        item.b = 0;
        item.flags = 0;

        fz_outline_iterator_insert(ctx, iter, &item);

        if (uriOut) {
            *uriOut = fz_strdup(ctx, uri);
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }

        fz_free(ctx, uri);
        uri = nullptr;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;

        if (uriOut && *uriOut) {
            fz_free(ctx, *uriOut);
            *uriOut = nullptr;
        }
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static bool DeleteBookmarkInPdf(EngineMupdf* epdf, const Vec<int>& path) {
    if (!epdf || !epdf->pdfdoc || path.Size() == 0) {
        return false;
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(ok);

    fz_try(ctx) {
        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (!MoveOutlineIteratorToPath(ctx, iter, path)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate outline item");
        }

        int rc = fz_outline_iterator_delete(ctx, iter);
        if (rc < 0) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to delete outline item");
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static bool DeleteBookmarkInPdfUnlocked(fz_context* ctx, EngineMupdf* epdf, const Vec<int>& path) {
    if (!ctx || !epdf || !epdf->pdfdoc || path.Size() == 0) {
        return false;
    }

    fz_outline_iterator* iter = nullptr;
    bool ok = false;

    fz_var(iter);
    fz_var(ok);

    fz_try(ctx) {
        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (!MoveOutlineIteratorToPath(ctx, iter, path)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate outline item");
        }

        int rc = fz_outline_iterator_delete(ctx, iter);
        if (rc < 0) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to delete outline item");
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }

    return ok;
}

static bool MoveBookmarkUpDownInPdf(EngineMupdf* epdf, const Vec<int>& path, bool moveDown, fz_outline* snapshot) {
    if (!epdf || !epdf->pdfdoc || path.Size() == 0 || !snapshot) {
        return false;
    }

    Vec<int> insertPath;
    Vec<int> deletePath;

    if (moveDown) {
        if (!BuildSiblingPath(path, 1, insertPath)) {
            return false;
        }

        CopyPath(path, deletePath);
    } else {
        if (!BuildSiblingPath(path, -1, insertPath)) {
            return false;
        }

        // Moving up inserts a clone before the previous sibling.
        // That shifts the original item one slot down.
        if (!BuildSiblingPath(path, 1, deletePath)) {
            return false;
        }
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(ok);

    fz_try(ctx) {
        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (moveDown) {
            if (!MoveOutlineIteratorAfterPath(ctx, iter, insertPath)) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate move-down insertion point");
            }
        } else {
            if (!MoveOutlineIteratorToPath(ctx, iter, insertPath)) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate move-up insertion point");
            }
        }

        InsertOutlineSnapshotAtCurrent(ctx, iter, snapshot);

        fz_drop_outline_iterator(ctx, iter);
        iter = nullptr;

        if (!DeleteBookmarkInPdfUnlocked(ctx, epdf, deletePath)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to delete original outline item after move");
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static bool MoveBookmarkAsChildInPdf(EngineMupdf* epdf, const Vec<int>& sourcePath, const Vec<int>& targetParentPath,
                                     fz_outline* snapshot) {
    if (!epdf || !epdf->pdfdoc || sourcePath.Size() == 0 || targetParentPath.Size() == 0 || !snapshot) {
        return false;
    }

    if (PathEquals(sourcePath, targetParentPath)) {
        return false;
    }

    // Do not allow moving a node into its own subtree.
    if (PathIsPrefixOf(sourcePath, targetParentPath)) {
        return false;
    }

    Vec<int> deletePath;

    if (PathIsPrefixOf(targetParentPath, sourcePath)) {
        // Inserting as the first child of targetParent shifts the existing
        // child list down by one. The source is somewhere inside that list.
        CopyPathWithIncrementAt(sourcePath, targetParentPath.Size(), deletePath);
    } else {
        CopyPath(sourcePath, deletePath);
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(ok);

    fz_try(ctx) {
        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (!MoveOutlineIteratorBelowPath(ctx, iter, targetParentPath)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate child insertion point");
        }

        InsertOutlineSnapshotAtCurrent(ctx, iter, snapshot);

        fz_drop_outline_iterator(ctx, iter);
        iter = nullptr;

        if (!DeleteBookmarkInPdfUnlocked(ctx, epdf, deletePath)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to delete original outline item after child move");
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static bool MoveBookmarkToRootEndInPdf(EngineMupdf* epdf, const Vec<int>& sourcePath, fz_outline* snapshot) {
    if (!epdf || !epdf->pdfdoc || sourcePath.Size() == 0 || !snapshot) {
        return false;
    }

    fz_context* ctx = epdf->Ctx();
    fz_outline_iterator* iter = nullptr;
    bool ok = false;

    EnterCriticalSection(&epdf->docLock);

    fz_var(iter);
    fz_var(ok);

    fz_try(ctx) {
        iter = fz_new_outline_iterator(ctx, epdf->_doc);
        if (!iter) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to create outline iterator");
        }

        if (!MoveOutlineIteratorToRootEnd(ctx, iter)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to locate root-end insertion point");
        }

        InsertOutlineSnapshotAtCurrent(ctx, iter, snapshot);

        fz_drop_outline_iterator(ctx, iter);
        iter = nullptr;

        if (!DeleteBookmarkInPdfUnlocked(ctx, epdf, sourcePath)) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "failed to delete original outline item after root move");
        }

        ok = true;
    }
    fz_always(ctx) {
        if (iter) {
            fz_drop_outline_iterator(ctx, iter);
            iter = nullptr;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }

    LeaveCriticalSection(&epdf->docLock);

    return ok;
}

static void MarkPdfBookmarksModified(EngineMupdf* epdf) {
    if (!epdf) {
        return;
    }

    epdf->modifiedPdfBookmarks = true;
}

// ================================================================
// Session outline mutations
// ================================================================

static fz_outline* CreateSessionOutlineNode(fz_context* ctx, fz_document* doc, const char* title, const char* uri,
                                            int pageNo) {
    if (!ctx || !doc || !title || title[0] == '\0' || !uri || pageNo <= 0) {
        return nullptr;
    }

    fz_outline* node = fz_new_outline(ctx);
    if (!node) {
        return nullptr;
    }

    fz_try(ctx) {
        node->title = fz_strdup(ctx, title);
        node->uri = fz_strdup(ctx, uri);

        node->page = fz_location_from_page_number(ctx, doc, pageNo - 1);
        node->x = 0;
        node->y = 0;

        node->is_open = 0;
        node->flags = 0;
        node->r = 0;
        node->g = 0;
        node->b = 0;
    }
    fz_catch(ctx) {
        fz_drop_outline(ctx, node);
        return nullptr;
    }

    return node;
}

static fz_outline* InsertSessionOutlineAtRootEnd(fz_context* ctx, fz_document* doc, fz_outline*& root,
                                                 const char* title, const char* uri, int pageNo) {
    fz_outline* newNode = CreateSessionOutlineNode(ctx, doc, title, uri, pageNo);
    if (!newNode) {
        return nullptr;
    }

    if (!root) {
        root = newNode;
        return newNode;
    }

    fz_outline* last = root;
    while (last->next) {
        last = last->next;
    }

    last->next = newNode;
    return newNode;
}

static fz_outline* InsertSessionOutlineAfterPath(fz_context* ctx, fz_document* doc, fz_outline*& root,
                                                 const Vec<int>& afterPath, const char* title, const char* uri,
                                                 int pageNo) {
    if (!ctx || !doc || !title || title[0] == '\0' || !uri || afterPath.Size() == 0) {
        return nullptr;
    }

    fz_outline* afterNode = root;

    for (int depth = 0; depth < afterPath.Size(); depth++) {
        int index = afterPath.At(depth);
        if (index < 0) {
            return nullptr;
        }

        while (afterNode && index > 0) {
            afterNode = afterNode->next;
            index--;
        }

        if (!afterNode) {
            return nullptr;
        }

        if (depth + 1 < afterPath.Size()) {
            afterNode = afterNode->down;
        }
    }

    fz_outline* newNode = CreateSessionOutlineNode(ctx, doc, title, uri, pageNo);
    if (!newNode) {
        return nullptr;
    }

    newNode->next = afterNode->next;
    afterNode->next = newNode;

    return newNode;
}

static fz_outline* InsertSessionOutlineChildAtPath(fz_context* ctx, fz_document* doc, fz_outline*& root,
                                                   const Vec<int>& parentPath, const char* title, const char* uri,
                                                   int pageNo) {
    if (!ctx || !doc || !root || !title || title[0] == '\0' || !uri || parentPath.Size() == 0) {
        return nullptr;
    }

    fz_outline* parentNode = root;

    for (int depth = 0; depth < parentPath.Size(); depth++) {
        int index = parentPath.At(depth);
        if (index < 0) {
            return nullptr;
        }

        while (parentNode && index > 0) {
            parentNode = parentNode->next;
            index--;
        }

        if (!parentNode) {
            return nullptr;
        }

        if (depth + 1 < parentPath.Size()) {
            parentNode = parentNode->down;
        }
    }

    fz_outline* newNode = CreateSessionOutlineNode(ctx, doc, title, uri, pageNo);
    if (!newNode) {
        return nullptr;
    }

    newNode->next = parentNode->down;
    parentNode->down = newNode;

    return newNode;
}

static bool RemoveSessionOutlineAtPath(fz_context* ctx, fz_outline*& root, const Vec<int>& path) {
    if (!ctx || !root || path.Size() == 0) {
        return false;
    }

    fz_outline** link = &root;

    for (int depth = 0; depth < path.Size(); depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return false;
        }

        while (*link && index > 0) {
            link = &((*link)->next);
            index--;
        }

        if (!*link) {
            return false;
        }

        if (depth + 1 < path.Size()) {
            link = &((*link)->down);
        }
    }

    fz_outline* doomed = *link;
    *link = doomed->next;

    // Important: fz_drop_outline() drops next and down recursively.
    // Detach next so we only delete this item and its child subtree,
    // not following siblings.
    doomed->next = nullptr;

    fz_drop_outline(ctx, doomed);
    return true;
}

static fz_outline** GetOutlineSiblingListForPath(fz_outline*& root, const Vec<int>& path) {
    if (!root || path.Size() == 0) {
        return nullptr;
    }

    fz_outline** link = &root;

    for (int depth = 0; depth < path.Size() - 1; depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return nullptr;
        }

        while (*link && index > 0) {
            link = &((*link)->next);
            index--;
        }

        if (!*link) {
            return nullptr;
        }

        link = &((*link)->down);
    }

    return link;
}

static fz_outline** GetOutlineLinkAtPath(fz_outline*& root, const Vec<int>& path) {
    if (!root || path.Size() == 0) {
        return nullptr;
    }

    fz_outline** link = &root;

    for (int depth = 0; depth < path.Size(); depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return nullptr;
        }

        while (*link && index > 0) {
            link = &((*link)->next);
            index--;
        }

        if (!*link) {
            return nullptr;
        }

        if (depth + 1 < path.Size()) {
            link = &((*link)->down);
        }
    }

    return link;
}

static bool MoveSessionOutlineUpDownAtPath(fz_outline*& root, const Vec<int>& path, bool moveDown) {
    fz_outline** firstLink = GetOutlineSiblingListForPath(root, path);
    if (!firstLink) {
        return false;
    }

    int index = path.At(path.Size() - 1);
    if (index < 0) {
        return false;
    }

    if (moveDown) {
        fz_outline** curLink = firstLink;

        for (int i = 0; i < index; i++) {
            if (!*curLink) {
                return false;
            }
            curLink = &((*curLink)->next);
        }

        fz_outline* cur = *curLink;
        if (!cur || !cur->next) {
            return false;
        }

        fz_outline* next = cur->next;
        cur->next = next->next;
        next->next = cur;
        *curLink = next;
        return true;
    }

    if (index <= 0) {
        return false;
    }

    fz_outline** prevLink = firstLink;

    for (int i = 0; i < index - 1; i++) {
        if (!*prevLink) {
            return false;
        }
        prevLink = &((*prevLink)->next);
    }

    fz_outline* prev = *prevLink;
    if (!prev || !prev->next) {
        return false;
    }

    fz_outline* cur = prev->next;
    prev->next = cur->next;
    cur->next = prev;
    *prevLink = cur;
    return true;
}

static bool MoveSessionOutlineAsChildAtPath(fz_outline*& root, const Vec<int>& sourcePath,
                                            const Vec<int>& targetParentPath) {
    if (!root || sourcePath.Size() == 0 || targetParentPath.Size() == 0) {
        return false;
    }

    if (PathEquals(sourcePath, targetParentPath)) {
        return false;
    }

    if (PathIsPrefixOf(sourcePath, targetParentPath)) {
        return false;
    }

    fz_outline** targetLink = GetOutlineLinkAtPath(root, targetParentPath);
    if (!targetLink || !*targetLink) {
        return false;
    }

    fz_outline* targetParent = *targetLink;

    fz_outline** sourceLink = GetOutlineLinkAtPath(root, sourcePath);
    if (!sourceLink || !*sourceLink) {
        return false;
    }

    fz_outline* source = *sourceLink;

    // Detach source from its old sibling list.
    *sourceLink = source->next;

    // Insert as first child of targetParent.
    source->next = targetParent->down;
    targetParent->down = source;

    return true;
}

static bool MoveSessionOutlineToRootEndAtPath(fz_outline*& root, const Vec<int>& sourcePath) {
    if (!root || sourcePath.Size() == 0) {
        return false;
    }

    fz_outline** sourceLink = GetOutlineLinkAtPath(root, sourcePath);
    if (!sourceLink || !*sourceLink) {
        return false;
    }

    fz_outline* source = *sourceLink;

    // Already root-level last item: no structural change needed.
    if (sourcePath.Size() == 1 && !source->next) {
        return true;
    }

    // Detach from old sibling list.
    *sourceLink = source->next;
    source->next = nullptr;

    if (!root) {
        root = source;
        return true;
    }

    fz_outline* last = root;
    while (last->next) {
        last = last->next;
    }

    last->next = source;
    return true;
}

// ================================================================
// TOC tree mutations
// ================================================================

static TocItem* AddRootTocItemInMemory(TocTree* tocTree, const char* title, int pageNo) {
    if (!tocTree || !tocTree->root || !title || title[0] == '\0' || pageNo <= 0) {
        return nullptr;
    }

    TocItem* item = new TocItem();
    item->title = str::Dup(title);
    item->pageNo = pageNo;

    // Root-level visible items should behave as roots in FindVisibleParentTreeItem().
    item->parent = nullptr;
    item->next = nullptr;
    item->child = nullptr;

    item->isOpenDefault = false;
    item->isOpenToggled = false;

    TocItem** link = &tocTree->root->child;
    while (*link) {
        link = &((*link)->next);
    }

    *link = item;
    return item;
}

static TocItem* AddTocItemAfterInMemory(TocItem* afterItem, const char* title, int pageNo) {
    if (!afterItem || !title || title[0] == '\0' || pageNo <= 0) {
        return nullptr;
    }

    auto* item = new TocItem();
    item->title = str::Dup(title);
    item->pageNo = pageNo;
    item->parent = afterItem->parent;
    item->next = afterItem->next;
    item->isOpenDefault = false;
    item->isOpenToggled = false;

    afterItem->next = item;

    return item;
}

static TocItem* AddChildTocItemInMemory(TocItem* parentItem, const char* title, int pageNo) {
    if (!parentItem || !title || title[0] == '\0' || pageNo <= 0) {
        return nullptr;
    }

    TocItem* item = new TocItem();
    item->title = str::Dup(title);
    item->pageNo = pageNo;

    item->parent = parentItem;
    item->next = parentItem->child;
    item->child = nullptr;

    item->isOpenDefault = false;
    item->isOpenToggled = false;

    parentItem->child = item;

    return item;
}

static bool RemoveTocItemAtPath(TocTree* tocTree, const Vec<int>& path) {
    if (!tocTree || !tocTree->root || path.Size() == 0) {
        return false;
    }

    TocItem** link = &tocTree->root->child;

    for (int depth = 0; depth < path.Size(); depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return false;
        }

        while (*link && index > 0) {
            link = &((*link)->next);
            index--;
        }

        if (!*link) {
            return false;
        }

        if (depth + 1 < path.Size()) {
            link = &((*link)->child);
        }
    }

    TocItem* doomed = *link;
    *link = doomed->next;

    // Detach sibling chain before delete. We want to delete this item and
    // its children, but not following siblings.
    doomed->next = nullptr;

    delete doomed;
    return true;
}

static TocItem** GetTocSiblingListForPath(TocTree* tocTree, const Vec<int>& path) {
    if (!tocTree || !tocTree->root || path.Size() == 0) {
        return nullptr;
    }

    TocItem** link = &tocTree->root->child;

    for (int depth = 0; depth < path.Size() - 1; depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return nullptr;
        }

        while (*link && index > 0) {
            link = &((*link)->next);
            index--;
        }

        if (!*link) {
            return nullptr;
        }

        link = &((*link)->child);
    }

    return link;
}

static TocItem** GetTocItemLinkAtPath(TocTree* tocTree, const Vec<int>& path) {
    if (!tocTree || !tocTree->root || path.Size() == 0) {
        return nullptr;
    }

    TocItem** link = &tocTree->root->child;

    for (int depth = 0; depth < path.Size(); depth++) {
        int index = path.At(depth);
        if (index < 0) {
            return nullptr;
        }

        while (*link && index > 0) {
            link = &((*link)->next);
            index--;
        }

        if (!*link) {
            return nullptr;
        }

        if (depth + 1 < path.Size()) {
            link = &((*link)->child);
        }
    }

    return link;
}

static void SetTocItemParentRecursive(TocItem* item, TocItem* parent) {
    if (!item) {
        return;
    }

    item->parent = parent;

    for (TocItem* child = item->child; child; child = child->next) {
        SetTocItemParentRecursive(child, item);
    }
}

static bool MoveTocItemUpDownAtPath(TocTree* tocTree, const Vec<int>& path, bool moveDown) {
    TocItem** firstLink = GetTocSiblingListForPath(tocTree, path);
    if (!firstLink) {
        return false;
    }

    int index = path.At(path.Size() - 1);
    if (index < 0) {
        return false;
    }

    if (moveDown) {
        TocItem** curLink = firstLink;

        for (int i = 0; i < index; i++) {
            if (!*curLink) {
                return false;
            }
            curLink = &((*curLink)->next);
        }

        TocItem* cur = *curLink;
        if (!cur || !cur->next) {
            return false;
        }

        TocItem* next = cur->next;
        cur->next = next->next;
        next->next = cur;
        *curLink = next;
        return true;
    }

    if (index <= 0) {
        return false;
    }

    TocItem** prevLink = firstLink;

    for (int i = 0; i < index - 1; i++) {
        if (!*prevLink) {
            return false;
        }
        prevLink = &((*prevLink)->next);
    }

    TocItem* prev = *prevLink;
    if (!prev || !prev->next) {
        return false;
    }

    TocItem* cur = prev->next;
    prev->next = cur->next;
    cur->next = prev;
    *prevLink = cur;
    return true;
}

static bool MoveTocItemAsChildAtPath(TocTree* tocTree, const Vec<int>& sourcePath, const Vec<int>& targetParentPath) {
    if (!tocTree || !tocTree->root || sourcePath.Size() == 0 || targetParentPath.Size() == 0) {
        return false;
    }

    if (PathEquals(sourcePath, targetParentPath)) {
        return false;
    }

    if (PathIsPrefixOf(sourcePath, targetParentPath)) {
        return false;
    }

    TocItem** targetLink = GetTocItemLinkAtPath(tocTree, targetParentPath);
    if (!targetLink || !*targetLink) {
        return false;
    }

    TocItem* targetParent = *targetLink;

    TocItem** sourceLink = GetTocItemLinkAtPath(tocTree, sourcePath);
    if (!sourceLink || !*sourceLink) {
        return false;
    }

    TocItem* source = *sourceLink;

    // Detach source from old sibling list.
    *sourceLink = source->next;

    // Insert as first child of targetParent.
    source->next = targetParent->child;
    targetParent->child = source;

    SetTocItemParentRecursive(source, targetParent);

    targetParent->isOpenDefault = true;
    targetParent->isOpenToggled = true;

    return true;
}

static bool MoveTocItemToRootEndAtPath(TocTree* tocTree, const Vec<int>& sourcePath) {
    if (!tocTree || !tocTree->root || sourcePath.Size() == 0) {
        return false;
    }

    TocItem** sourceLink = GetTocItemLinkAtPath(tocTree, sourcePath);
    if (!sourceLink || !*sourceLink) {
        return false;
    }

    TocItem* source = *sourceLink;

    // Already root-level last item: no structural change needed.
    if (sourcePath.Size() == 1 && !source->next) {
        return true;
    }

    // Detach from old sibling list.
    *sourceLink = source->next;
    source->next = nullptr;

    TocItem** link = &tocTree->root->child;
    while (*link) {
        link = &((*link)->next);
    }

    *link = source;

    SetTocItemParentRecursive(source, nullptr);
    return true;
}
