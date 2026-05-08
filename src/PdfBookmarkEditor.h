#pragma once

class EngineBase;
struct TocItem;
struct fz_outline;
struct TocTree;

class PdfBookmarkEditor {
  public:
    explicit PdfBookmarkEditor(EngineBase* engine);

    TocTree* GetOrCreateTocTree();

    bool AddRootBookmark(const char* title, int pageNo);

    bool CanEditBookmarks() const;
    bool CanRenameBookmarks() const;
    bool CanAddBookmarks() const;
    bool CanDeleteBookmarks() const;
    bool CanMoveBookmarks() const;
    bool CanMoveBookmarkUp(TocItem* item) const;
    bool CanMoveBookmarkDown(TocItem* item) const;
    bool CanMoveBookmarkAsChild(TocItem* sourceItem, TocItem* targetParentItem) const;
    bool CanMoveBookmarkToRootEnd(TocItem* sourceItem) const;

    bool RenameBookmark(TocItem* item, const char* newTitle);
    bool AddBookmarkAfter(TocItem* afterItem, const char* title, int pageNo);
    bool AddChildBookmark(TocItem* parentItem, const char* title, int pageNo);
    bool DeleteBookmark(TocItem* item);
    bool MoveBookmarkUp(TocItem* item);
    bool MoveBookmarkDown(TocItem* item);
    bool MoveBookmarkUpDown(TocItem* item, bool moveDown);
    bool MoveBookmarkAsChild(TocItem* sourceItem, TocItem* targetParentItem);
    bool MoveBookmarkToRootEnd(TocItem* sourceItem);

  private:
    EngineBase* engine = nullptr;

    bool GetTocItemPath(TocItem* item, Vec<int>& pathOut) const;
    bool FindTocItemPathRecursive(TocItem* root, TocItem* wanted, Vec<int>& pathOut) const;

    fz_outline* FindOutlineByPath(const Vec<int>& path) const;
};
