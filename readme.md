## SumatraPDF Experimental Fork

This fork adds experimental PDF-focused features.

### Added Features

#### Dockable Annotation Editor and Toolbar Improvements

- Added a dockable annotation editor panel with close, dock, undock, and resize controls.
- Added toolbar buttons for Facing View, Save PDF Changes, and Edit Annotations.
- Added a unified dirty-state indicator for unsaved PDF changes.
- Made the Edit Annotations button toggle the annotation editor.
- Fixed annotation color changes so current opacity is preserved.

#### Editable PDF Bookmarks

- Added basic PDF bookmark editing through a new `PdfBookmarkEditor`.
- Supports adding, renaming, deleting, moving, and drag-and-drop nesting of bookmarks.
- Allows saving bookmark changes to the current PDF or to a new PDF.
- Allows opening the bookmarks panel for editable PDFs even when no bookmarks exist yet.
- Added `Ctrl+Shift+B` to create a bookmark for the current page, using selected text as the title when available.
- Bookmark edits and annotation edits now share one unified PDF Changes dirty state.

#### Initial Read Aloud Support Using Windows SAPI

- Added Read Aloud support on Windows using installed SAPI voices.
- Reads selected text, or the current page when no text is selected.
- The same toolbar button starts and stops speech.
- Toolbar icon updates automatically when speech starts, stops, or finishes.
- Voice selection is available from the Read Aloud toolbar dropdown.
- Installed voices are sorted by language when available.
- Basic PDF text cleanup improves speech flow by fixing wrapped words, visual line breaks, and line-based pauses.
- Speech stops automatically when the source tab or window is closed.