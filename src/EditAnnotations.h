/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct EditAnnotationsWindow;

enum class EditAnnotFocus {
    Default,
    Edit,
    List,
};

void ShowEditAnnotationsWindow(WindowTab*, Annotation*, EditAnnotFocus focus = EditAnnotFocus::Default);
bool CloseAndDeleteEditAnnotationsWindow(WindowTab*);
void DeleteAnnotationAndUpdateUI(WindowTab*, Annotation*);
void SetSelectedAnnotation(WindowTab*, Annotation*, bool isNew = false, EditAnnotFocus focus = EditAnnotFocus::Default);
void UpdateAnnotationsList(EditAnnotationsWindow*);
void NotifyAnnotationsChanged(EditAnnotationsWindow*);

bool IsEditAnnotationsDockedAndVisible(WindowTab* tab);
bool HasDockedEditAnnotationsWindow(WindowTab* tab);
HWND GetEditAnnotationsHwnd(WindowTab* tab);
void UpdateDockedEditAnnotationsVisibility(MainWindow* win);
void UpdateEditAnnotationsThemeForWindow(MainWindow* win);
void ToggleDockEditAnnotationsWindow(WindowTab* tab);
bool CloseAndDeleteEditAnnotationsWindow(WindowTab* tab);
