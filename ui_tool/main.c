#include "engine/core/engine.h"
#include "engine/core/ui.h"
#include "engine/core/ui_containers.h"
#include "engine/core/ui_document.h"
#include "engine/core/ui_editor.h"
#include "engine/core/ui_layout.h"
#include "engine/core/ui_menu.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAYOUT_PATH "ui_tool/layout.ui"
#define UNDO_CAPACITY 32
#define MARGIN 8
#define PALETTE_WIDTH 176
#define INSPECTOR_WIDTH 236
#define STATUS_HEIGHT 22
#define SNAP_THRESHOLD 5
#define UI_ELEMENT_MINIMUM_SIZE 8

typedef enum ToolAction
{
    ACTION_ALIGN_LEFT,
    ACTION_ALIGN_CENTRE,
    ACTION_ALIGN_RIGHT,
    ACTION_ALIGN_TOP,
    ACTION_ALIGN_MIDDLE,
    ACTION_ALIGN_BOTTOM,
    ACTION_FILL_WIDTH,
    ACTION_ROW_HEIGHT,
    ACTION_STACK,
    ACTION_DUPLICATE,
    ACTION_RAISE,
    ACTION_LOWER,
    ACTION_DELETE,
    ACTION_UNDO,
    ACTION_ZOOM,
    ACTION_SAVE,
    ACTION_LOAD
} ToolAction;

#define ZOOM_MAXIMUM 8

typedef struct ToolButton
{
    const char *label;
    ToolAction action;
} ToolButton;

// Related buttons share one indent and sit flush inside it; the clearance is between the groups.
typedef struct ToolGroup
{
    const ToolButton *buttons;
    int count;
} ToolGroup;

static const ToolButton ALIGN_ACROSS[] = {
    {"L", ACTION_ALIGN_LEFT}, {"C", ACTION_ALIGN_CENTRE}, {"R", ACTION_ALIGN_RIGHT}};
static const ToolButton ALIGN_DOWN[] = {
    {"T", ACTION_ALIGN_TOP}, {"M", ACTION_ALIGN_MIDDLE}, {"B", ACTION_ALIGN_BOTTOM}};
static const ToolButton SIZING[] = {
    {"Fill", ACTION_FILL_WIDTH}, {"Row", ACTION_ROW_HEIGHT}, {"Stack", ACTION_STACK}};
static const ToolButton EDITING[] = {{"Dup", ACTION_DUPLICATE},
                                     {"Raise", ACTION_RAISE},
                                     {"Lower", ACTION_LOWER},
                                     {"Delete", ACTION_DELETE}};
static const ToolButton HISTORY[] = {{"Undo", ACTION_UNDO}};
static const ToolButton VIEW[] = {{"1x", ACTION_ZOOM}};
static const ToolButton FILING[] = {{"Save", ACTION_SAVE}, {"Load", ACTION_LOAD}};

static const ToolGroup TOOLBAR[] = {
    {ALIGN_ACROSS, 3}, {ALIGN_DOWN, 3}, {SIZING, 3}, {EDITING, 4},
    {HISTORY, 1},      {VIEW, 1},       {FILING, 2},
};
#define TOOLBAR_COUNT (sizeof TOOLBAR / sizeof TOOLBAR[0])

typedef struct Tool
{
    UiContext ui;
    UiDocument document;
    UiRectEditor rectEditor;
    UiPanel palette;
    UiPanel inspector;
    EngineInput input;

    UiRect workspace;
    UiRect labelField; // where the inspector drew the label editor this frame
    char entry[10][16]; // text behind the numeric boxes: x, y, w, h, min, max, surface
    UiRect content;    // content area of the document, in surface pixels
    Vector2 origin;    // top-left of the whole surface, on screen
    Vector2 pointer;   // the mouse, in surface pixels
    bool pointerInWorkspace;
    RenderTexture canvas; // the surface drawn at 1:1, blitted at an integer zoom
    int zoom;
    Vector2 pan; // offset from the centred position, dragged with the middle button
    bool panning;
    int testWidth; // surface size being previewed; the authored size when not resizing
    int testHeight;
    bool resizing;
    Vector2 resizeOrigin;
    Vector2 contextPoint;
    UiSnapResult guides;

    UiRect *scratch;
    size_t scratchCapacity;

    UiDocument undo[UNDO_CAPACITY];
    size_t undoCount;

    int selected;
    int creating;
    int gridStep;
    bool preview;
    bool snap;
    bool grid;
    bool quit;
    char status[96];

    bool smoke;
    int frames;
    int failures;
} Tool;

static int MinInt(int a, int b) { return a < b ? a : b; }
static int MaxInt(int a, int b) { return a > b ? a : b; }

static bool PointIn(UiRect rect, Vector2 point)
{
    return CheckCollisionPointRec(
        point, (Rectangle){(float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height});
}

static int ClampInt(int value, int minimum, int maximum)
{
    if (maximum < minimum)
        return minimum;
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static const char *ElementName(UiElementType type)
{
    static const char *names[] = {"Button", "Slider", "Checkbox", "Label", "Indent", "Window"};
    return type >= 0 && type < UI_ELEMENT_COUNT ? names[type] : "Element";
}

static const char *StyleName(UiSurfaceStyle style)
{
    static const char *names[] = {"Screen", "Panel", "Window"};
    return style >= 0 && style < UI_SURFACE_STYLE_COUNT ? names[style] : "Panel";
}

static void Say(Tool *tool, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(tool->status, sizeof tool->status, format, arguments);
    va_end(arguments);
}

static UiElement *Selected(Tool *tool)
{
    if (tool->selected < 0 || tool->selected >= (int)tool->document.count)
        return NULL;
    return &tool->document.elements[tool->selected];
}

static UiRect ContentBounds(const Tool *tool)
{
    return (UiRect){0, 0, tool->document.surfaceWidth, tool->document.surfaceHeight};
}

// The size being shown. Equal to the authored size unless the surface is being resized to test it.
static int TestWidth(const Tool *tool)
{
    return tool->testWidth > 0 ? tool->testWidth : tool->document.surfaceWidth;
}

static int TestHeight(const Tool *tool)
{
    return tool->testHeight > 0 ? tool->testHeight : tool->document.surfaceHeight;
}

// Anchors resolve against the authored size, so editing is only meaningful at that size.
static bool Testing(const Tool *tool)
{
    return TestWidth(tool) != tool->document.surfaceWidth ||
           TestHeight(tool) != tool->document.surfaceHeight;
}

static void ClearUndo(Tool *tool)
{
    for (size_t i = 0; i < tool->undoCount; i++)
        UiDocumentFree(&tool->undo[i]);
    tool->undoCount = 0;
}

static void PushUndo(Tool *tool)
{
    UiDocument snapshot = {0};
    if (!UiDocumentCopy(&snapshot, &tool->document))
        return;
    if (tool->undoCount == UNDO_CAPACITY)
    {
        UiDocumentFree(&tool->undo[0]);
        memmove(&tool->undo[0], &tool->undo[1], (UNDO_CAPACITY - 1) * sizeof(tool->undo[0]));
        tool->undoCount--;
    }
    tool->undo[tool->undoCount++] = snapshot;
}

static void Undo(Tool *tool)
{
    if (!tool->undoCount)
    {
        Say(tool, "Nothing to undo");
        return;
    }
    UiDocument snapshot = tool->undo[--tool->undoCount];
    UiDocumentFree(&tool->document);
    tool->document = snapshot;
    tool->undo[tool->undoCount] = (UiDocument){0};
    if (tool->selected >= (int)tool->document.count)
        tool->selected = (int)tool->document.count - 1;
    Say(tool, "Undo");
}

static UiRect ContainerContent(const Tool *tool, const UiElement *element)
{
    return UiDocumentContainerContent(&tool->ui, element, element->rect);
}

static bool IsContainer(UiElementType type)
{
    return type == UI_ELEMENT_INDENT || type == UI_ELEMENT_WINDOW;
}

// The rectangle an element is arranged and snapped within: its innermost container, or the surface.
static UiRect ArrangeBounds(const Tool *tool, UiRect rect, int ignore, int *gap)
{
    UiRect bounds = ContentBounds(tool);
    *gap = tool->ui.theme.containerGap;
    Vector2 centre = {(float)(rect.x + rect.width / 2), (float)(rect.y + rect.height / 2)};
    long best = 0;
    for (size_t i = 0; i < tool->document.count; i++)
    {
        const UiElement *element = &tool->document.elements[i];
        if ((int)i == ignore || !IsContainer(element->type) || !PointIn(element->rect, centre))
            continue;
        UiRect content = ContainerContent(tool, element);
        long area = (long)content.width * content.height;
        if (content.width <= 0 || content.height <= 0 || (best && area >= best))
            continue;
        best = area;
        bounds = content;
        *gap = 0;
    }
    return bounds;
}

// What a control needs to still show its own label. The editor works this out and writes it into
// the element's minimum, where it is visible in Properties and can be typed over.
static void LabelMinimum(Tool *tool, const UiElement *element, int *width, int *height)
{
    const UiTheme *theme = &tool->ui.theme;
    int text = UiTextWidth(&tool->ui, element->label);
    switch (element->type)
    {
        case UI_ELEMENT_LABEL:
            *width = text;
            *height = theme->fontSize;
            return;
        case UI_ELEMENT_CHECKBOX:
            *width = theme->checkboxSize + theme->padding + text;
            *height = theme->itemHeight;
            return;
        case UI_ELEMENT_SLIDER:
            *width = theme->sliderKnobWidth * 3;
            *height = theme->itemHeight;
            return;
        case UI_ELEMENT_INDENT:
            *width = theme->indentWidth * 2 + theme->itemHeight;
            *height = theme->indentWidth * 2 + theme->itemHeight;
            return;
        case UI_ELEMENT_WINDOW:
            *width = theme->frameWidth * 2 + theme->padding * 2 + text;
            *height = theme->frameWidth * 2 + theme->titleHeight + theme->itemHeight;
            return;
        default:
            *width = text + theme->padding * 2 + (theme->indentWidth + theme->outsetWidth) * 2;
            *height = theme->itemHeight;
            return;
    }
}

static UiRect DefaultRect(const Tool *tool, UiElementType type, int x, int y)
{
    const UiTheme *theme = &tool->ui.theme;
    UiRect bounds = ContentBounds(tool);
    int rows = type == UI_ELEMENT_INDENT ? 3 : type == UI_ELEMENT_WINDOW ? 5 : 1;
    int height = type == UI_ELEMENT_WINDOW ? theme->titleHeight + theme->frameWidth * 2 + rows * 12
                                           : rows * theme->itemHeight;
    UiRect rect = {x, y, MinInt(140, bounds.width), height};
    return UiClampRect(rect, bounds, 8);
}

static void AddAt(Tool *tool, UiElementType type, int x, int y)
{
    UiRect rect = DefaultRect(tool, type, x, y);
    if (tool->snap)
    {
        int gap = 0;
        UiRect bounds = ArrangeBounds(tool, rect, -1, &gap);
        UiSnapConfig config = {bounds,  NULL, 0, tool->gridStep, SNAP_THRESHOLD, gap,
                               tool->ui.theme.itemHeight};
        UiSnapRect(&rect, UI_HANDLE_MOVE, config);
        rect = UiClampRect(rect, ContentBounds(tool), 8);
    }
    PushUndo(tool);
    UiElement *element = UiDocumentAdd(&tool->document, type, rect, ElementName(type));
    if (element)
    {
        LabelMinimum(tool, element, &element->minWidth, &element->minHeight);
        tool->selected = (int)tool->document.count - 1;
        Say(tool, "Added %s at %d,%d  minimum %dx%d", ElementName(type), rect.x, rect.y,
            element->minWidth, element->minHeight);
    }
}

static void Save(Tool *tool)
{
    bool ok = UiDocumentSave(&tool->document, LAYOUT_PATH);
    Say(tool, "%s %s", ok ? "Saved" : "Save failed", LAYOUT_PATH);
}

static void Load(Tool *tool)
{
    PushUndo(tool);
    bool ok = UiDocumentLoad(&tool->document, LAYOUT_PATH);
    tool->selected = ok && tool->document.count ? 0 : -1;
    Say(tool, "%s %s", ok ? "Loaded" : "Load failed", LAYOUT_PATH);
}

// Integer factors only: a UI built from single-pixel bevels cannot survive a fractional scale.
static void SetZoom(Tool *tool, int zoom)
{
    int next = ClampInt(zoom, 1, ZOOM_MAXIMUM);
    if (next == tool->zoom)
        return;
    // Keep whatever is under the middle of the workspace where it is.
    tool->pan.x = tool->pan.x * (float)next / (float)tool->zoom;
    tool->pan.y = tool->pan.y * (float)next / (float)tool->zoom;
    tool->zoom = next;
    Say(tool, "Zoom %dx", tool->zoom);
}

static void RunAction(Tool *tool, ToolAction action)
{
    if (action == ACTION_ZOOM)
    {
        SetZoom(tool, tool->zoom * 2 > ZOOM_MAXIMUM ? 1 : tool->zoom * 2);
        return;
    }
    if (action == ACTION_SAVE)
    {
        Save(tool);
        return;
    }
    if (action == ACTION_LOAD)
    {
        Load(tool);
        return;
    }
    if (action == ACTION_UNDO)
    {
        Undo(tool);
        return;
    }

    UiElement *element = Selected(tool);
    if (!element)
    {
        Say(tool, "Select an element first");
        return;
    }
    // Alignment happens inside whatever contains the element, flush to its bezel.
    int gap = 0;
    UiRect bounds = ArrangeBounds(tool, element->rect, tool->selected, &gap);
    const char *where = gap ? "the surface" : "its container";
    PushUndo(tool);
    switch (action)
    {
        case ACTION_ALIGN_LEFT:
            element->rect.x = bounds.x;
            Say(tool, "Aligned left in %s", where);
            break;
        case ACTION_ALIGN_CENTRE:
            element->rect.x = bounds.x + (bounds.width - element->rect.width) / 2;
            Say(tool, "Centred horizontally in %s", where);
            break;
        case ACTION_ALIGN_RIGHT:
            element->rect.x = bounds.x + bounds.width - element->rect.width;
            Say(tool, "Aligned right in %s", where);
            break;
        case ACTION_ALIGN_TOP:
            element->rect.y = bounds.y;
            Say(tool, "Aligned top in %s", where);
            break;
        case ACTION_ALIGN_MIDDLE:
            element->rect.y = bounds.y + (bounds.height - element->rect.height) / 2;
            Say(tool, "Centred vertically in %s", where);
            break;
        case ACTION_ALIGN_BOTTOM:
            element->rect.y = bounds.y + bounds.height - element->rect.height;
            Say(tool, "Aligned bottom in %s", where);
            break;
        case ACTION_FILL_WIDTH:
            element->rect.x = bounds.x;
            element->rect.width = bounds.width;
            Say(tool, "Filled the width of %s", where);
            break;
        case ACTION_ROW_HEIGHT:
            element->rect.height = tool->ui.theme.itemHeight;
            Say(tool, "Set one row high (%d px)", tool->ui.theme.itemHeight);
            break;
        case ACTION_STACK:
            if (tool->selected > 0)
            {
                UiRect above = tool->document.elements[tool->selected - 1].rect;
                element->rect.x = above.x;
                element->rect.width = above.width;
                element->rect.y = above.y + above.height + gap;
                Say(tool, "Stacked under %s with a %d px gap",
                    ElementName(tool->document.elements[tool->selected - 1].type), gap);
            }
            else
                Say(tool, "Nothing above to stack under");
            break;
        case ACTION_DUPLICATE:
        {
            UiElement copy = *element;
            copy.rect.y += copy.rect.height + gap;
            UiElement *added = UiDocumentAdd(&tool->document, copy.type, copy.rect, copy.label);
            if (added)
            {
                *added = copy;
                tool->selected = (int)tool->document.count - 1;
                element = added;
                Say(tool, "Duplicated %s", ElementName(copy.type));
            }
            break;
        }
        case ACTION_RAISE:
            if (UiDocumentSwap(&tool->document, (size_t)tool->selected, (size_t)tool->selected + 1))
            {
                tool->selected++;
                Say(tool, "Raised");
            }
            element = NULL;
            break;
        case ACTION_LOWER:
            if (tool->selected > 0 &&
                UiDocumentSwap(&tool->document, (size_t)tool->selected, (size_t)tool->selected - 1))
            {
                tool->selected--;
                Say(tool, "Lowered");
            }
            element = NULL;
            break;
        case ACTION_DELETE:
            if (UiDocumentRemove(&tool->document, (size_t)tool->selected))
            {
                tool->selected = MinInt(tool->selected, (int)tool->document.count - 1);
                Say(tool, "Deleted");
            }
            element = NULL;
            break;
        default:
            break;
    }
    if (element)
        element->rect = UiClampRect(element->rect, bounds, 8);
}

static void MenuAddButton(void *user)
{
    Tool *tool = user;
    AddAt(tool, UI_ELEMENT_BUTTON, (int)tool->contextPoint.x, (int)tool->contextPoint.y);
}
static void MenuAddSlider(void *user)
{
    Tool *tool = user;
    AddAt(tool, UI_ELEMENT_SLIDER, (int)tool->contextPoint.x, (int)tool->contextPoint.y);
}
static void MenuAddCheckbox(void *user)
{
    Tool *tool = user;
    AddAt(tool, UI_ELEMENT_CHECKBOX, (int)tool->contextPoint.x, (int)tool->contextPoint.y);
}
static void MenuAddLabel(void *user)
{
    Tool *tool = user;
    AddAt(tool, UI_ELEMENT_LABEL, (int)tool->contextPoint.x, (int)tool->contextPoint.y);
}
static void MenuAddIndent(void *user)
{
    Tool *tool = user;
    AddAt(tool, UI_ELEMENT_INDENT, (int)tool->contextPoint.x, (int)tool->contextPoint.y);
}
static void MenuAddWindow(void *user)
{
    Tool *tool = user;
    AddAt(tool, UI_ELEMENT_WINDOW, (int)tool->contextPoint.x, (int)tool->contextPoint.y);
}
static void MenuAlignLeft(void *user) { RunAction(user, ACTION_ALIGN_LEFT); }
static void MenuAlignCentre(void *user) { RunAction(user, ACTION_ALIGN_CENTRE); }
static void MenuAlignRight(void *user) { RunAction(user, ACTION_ALIGN_RIGHT); }
static void MenuFillWidth(void *user) { RunAction(user, ACTION_FILL_WIDTH); }
static void MenuRowHeight(void *user) { RunAction(user, ACTION_ROW_HEIGHT); }
static void MenuStack(void *user) { RunAction(user, ACTION_STACK); }
static void MenuDuplicate(void *user) { RunAction(user, ACTION_DUPLICATE); }
static void MenuDelete(void *user) { RunAction(user, ACTION_DELETE); }
static void MenuSave(void *user) { RunAction(user, ACTION_SAVE); }
static void MenuLoad(void *user) { RunAction(user, ACTION_LOAD); }

static const UiMenuItem addItems[] = {
    {"Button", true, NULL, MenuAddButton},     {"Slider", true, NULL, MenuAddSlider},
    {"Checkbox", true, NULL, MenuAddCheckbox}, {"Label", true, NULL, MenuAddLabel},
    {"Indent", true, NULL, MenuAddIndent},     {"Window", true, NULL, MenuAddWindow},
};
static const UiMenu addMenu = {addItems, 6, 150, NULL};
static const UiMenuItem arrangeItems[] = {
    {"Align left", true, NULL, MenuAlignLeft},    {"Centre", true, NULL, MenuAlignCentre},
    {"Align right", true, NULL, MenuAlignRight},  {"Fill width", true, NULL, MenuFillWidth},
    {"One row high", true, NULL, MenuRowHeight},  {"Stack under", true, NULL, MenuStack},
};
static const UiMenu arrangeMenu = {arrangeItems, 6, 170, NULL};
static const UiMenuItem canvasItems[] = {
    {"Add", true, &addMenu, NULL},         {"Arrange", true, &arrangeMenu, NULL},
    {"Duplicate", true, NULL, MenuDuplicate}, {"Delete", true, NULL, MenuDelete},
    {"Save", true, NULL, MenuSave},        {"Load", true, NULL, MenuLoad},
};
static const UiMenu canvasMenu = {canvasItems, 6, 160, NULL};

static int ButtonWidth(Tool *tool, const char *label)
{
    const UiTheme *theme = &tool->ui.theme;
    int text = UiTextWidth(&tool->ui, label);
    return MaxInt(theme->itemHeight, text + theme->padding * 2);
}

static const char *ButtonLabel(const Tool *tool, const ToolButton *button, char *scratch,
                               size_t capacity)
{
    if (button->action != ACTION_ZOOM)
        return button->label;
    snprintf(scratch, capacity, "%dx", tool->zoom);
    return scratch;
}

// Groups wrap onto as many rows as the toolbar width needs, so no control is ever cut off.
static int Toolbar(Tool *tool, UiRect bounds, bool draw)
{
    const UiTheme *theme = &tool->ui.theme;
    int x = bounds.x;
    int y = bounds.y;
    int rowHeight = theme->itemHeight + theme->indentWidth * 2;
    for (size_t g = 0; g < TOOLBAR_COUNT; g++)
    {
        const ToolGroup *group = &TOOLBAR[g];
        int cell = 0;
        for (int i = 0; i < group->count; i++)
        {
            char scratch[8];
            cell = MaxInt(cell, ButtonWidth(tool, ButtonLabel(tool, &group->buttons[i], scratch,
                                                              sizeof scratch)));
        }
        int width = cell * group->count + theme->indentWidth * 2;
        if (x + width > bounds.x + bounds.width && x > bounds.x)
        {
            x = bounds.x;
            y += rowHeight + theme->containerGap;
        }
        if (draw)
        {
            UiRect well = {x, y, width, rowHeight};
            UiDrawIndent(&tool->ui, well);
            UiRect inner = UiRectInset(well, theme->indentWidth);
            for (int i = 0; i < group->count; i++)
            {
                char scratch[8];
                const char *label = ButtonLabel(tool, &group->buttons[i], scratch, sizeof scratch);
                UiRect button = {inner.x + i * cell, inner.y, cell, inner.height};
                if (UiButtonBare(&tool->ui, button, label))
                    RunAction(tool, group->buttons[i].action);
            }
        }
        x += width + theme->containerGap;
    }
    return y + rowHeight - bounds.y;
}

static void DrawPaletteContents(UiContext *ui, UiRect content, void *user)
{
    Tool *tool = user;
    UiLayout column = UiColumn(ui, content);
    int chrome = 2 * (ui->theme.containerGap + ui->theme.indentWidth);
    UiRect group = UiLayoutNext(ui, &column, UI_ELEMENT_COUNT * ui->theme.itemHeight + chrome);
    UiRect types = UiRectInset(UiRectInset(group, ui->theme.containerGap), ui->theme.indentWidth);
    UiDrawIndent(ui, UiRectInset(group, ui->theme.containerGap));
    for (int i = 0; i < UI_ELEMENT_COUNT; i++)
    {
        UiRect row = {types.x, types.y + i * ui->theme.itemHeight, types.width,
                      ui->theme.itemHeight};
        UiButtonFlags flags = UI_BUTTON_BARE;
        if (tool->creating == i)
            flags = (UiButtonFlags)(flags | UI_BUTTON_DOWN);
        if (UiButtonEx(ui, row, ElementName((UiElementType)i), flags))
        {
            tool->creating = tool->creating == i ? -1 : i;
            if (tool->creating >= 0)
                Say(tool, "Click the surface to place a %s", ElementName((UiElementType)i));
            else
                Say(tool, "Placement cancelled");
        }
    }

    // Snap, Grid and Preview are one related set: one indent, three flush toggles.
    UiRect well = UiLayoutNext(ui, &column, ui->theme.itemHeight * 2 + ui->theme.indentWidth * 2);
    UiDrawIndent(ui, well);
    UiRect inner = UiRectInset(well, ui->theme.indentWidth);
    bool *values[] = {&tool->snap, &tool->grid};
    const char *labels[] = {"Snap", "Grid"};
    for (int i = 0; i < 2; i++)
    {
        UiRect half = {inner.x + i * (inner.width / 2), inner.y, inner.width / 2,
                       ui->theme.itemHeight};
        UiButtonFlags flags = (UiButtonFlags)(UI_BUTTON_BARE | UI_BUTTON_TOGGLE_VISUAL);
        if (*values[i])
            flags = (UiButtonFlags)(flags | UI_BUTTON_DOWN);
        if (UiButtonEx(ui, half, labels[i], flags))
        {
            *values[i] = !*values[i];
            Say(tool, "%s %s", labels[i], *values[i] ? "on" : "off");
        }
    }
    UiButtonFlags previewFlags = (UiButtonFlags)(UI_BUTTON_BARE | UI_BUTTON_TOGGLE_VISUAL);
    if (tool->preview)
        previewFlags = (UiButtonFlags)(previewFlags | UI_BUTTON_DOWN);
    if (UiButtonEx(ui, (UiRect){inner.x, inner.y + ui->theme.itemHeight, inner.width,
                                ui->theme.itemHeight},
                   "Preview", previewFlags))
    {
        tool->preview = !tool->preview;
        Say(tool, tool->preview ? "Preview: widgets are live" : "Editing");
    }
}

// A real text box you can type into, select in and paste into, with steppers beside it. The buffer
// tracks the value while the box is not being edited, so both routes stay in step.
static bool NumberRow(UiContext *ui, UiRect rect, const char *name, int *value, char *buffer,
                      size_t capacity, int step, int minimum, int maximum)
{
    int side = ui->theme.itemHeight;
    UiRect label = {rect.x, rect.y, side, rect.height};
    UiRect box = {rect.x + side, rect.y, MaxInt(0, rect.width - side * 3 - ui->theme.containerGap),
                  rect.height};
    // The pair of steppers is one related set, so they share an indent and touch.
    UiRect steppers = {box.x + box.width + ui->theme.containerGap, rect.y, side * 2, rect.height};
    UiLabel(ui, label, name);
    if (!UiTextFieldFocused(ui, buffer))
        snprintf(buffer, capacity, "%d", *value);

    bool changed = false;
    if (UiTextField(ui, box, buffer, capacity))
    {
        char *end = NULL;
        long typed = strtol(buffer, &end, 10);
        if (end != buffer)
        {
            *value = ClampInt((int)typed, minimum, maximum);
            changed = true;
        }
    }
    UiDrawIndent(ui, steppers);
    UiRect inner = UiRectInset(steppers, ui->theme.indentWidth);
    if (UiButtonBare(ui, (UiRect){inner.x, inner.y, inner.width / 2, inner.height}, "-"))
    {
        *value = ClampInt(*value - step, minimum, maximum);
        changed = true;
    }
    if (UiButtonBare(ui,
                     (UiRect){inner.x + inner.width / 2, inner.y, inner.width - inner.width / 2,
                              inner.height},
                     "+"))
    {
        *value = ClampInt(*value + step, minimum, maximum);
        changed = true;
    }
    return changed;
}

static void TextAlignmentRow(Tool *tool, UiRect row, const char *label, UiAlign *alignment,
                             const char *const names[3])
{
    UiContext *ui = &tool->ui;
    int labelWidth = UiTextWidth(ui, label) + ui->theme.padding;
    UiLabel(ui, (UiRect){row.x, row.y, labelWidth, row.height}, label);
    UiRect well = {row.x + labelWidth, row.y, MaxInt(0, row.width - labelWidth), row.height};
    UiDrawIndent(ui, well);
    UiRect inner = UiRectInset(well, ui->theme.indentWidth);
    for (int i = 0; i < 3; i++)
    {
        // Radio semantics: the selected choice stays down even if clicked again.
        UiButtonFlags flags = (UiButtonFlags)(UI_BUTTON_BARE |
            (*alignment == (UiAlign)i ? UI_BUTTON_DOWN : UI_BUTTON_TOGGLE_VISUAL));
        if (UiButtonEx(ui, UiLayoutCell(inner, UI_LAYOUT_HORIZONTAL, i, 3, 0), names[i], flags) &&
            *alignment != (UiAlign)i)
        {
            PushUndo(tool);
            *alignment = (UiAlign)i;
        }
    }
}

static void DrawInspectorContents(UiContext *ui, UiRect content, void *user)
{
    Tool *tool = user;
    UiLayout column = UiColumn(ui, UiRectInset(content, ui->theme.containerGap));
    UiElement *element = Selected(tool);
    if (!element)
        UiLabel(ui, UiLayoutNext(ui, &column, 0), "Nothing selected");
    else
    {
        char header[64];
        snprintf(header, sizeof header, "%s  %d of %d", ElementName(element->type),
                 tool->selected + 1, (int)tool->document.count);
        UiLabel(ui, UiLayoutNext(ui, &column, 0), header);
        tool->labelField = UiLayoutNext(ui, &column, 0);
        int wasWidth = 0;
        int wasHeight = 0;
        LabelMinimum(tool, element, &wasWidth, &wasHeight);
        if (UiTextField(ui, tool->labelField, element->label, sizeof element->label))
        {
            // A minimum still matching the old label is the editor's, so it follows the new one.
            int nowWidth = 0;
            int nowHeight = 0;
            LabelMinimum(tool, element, &nowWidth, &nowHeight);
            if (element->minWidth == wasWidth)
                element->minWidth = nowWidth;
            if (element->minHeight == wasHeight)
                element->minHeight = nowHeight;
        }
        UiRect bounds = ContentBounds(tool);
        int step = tool->snap && tool->gridStep > 1 ? tool->gridStep : 1;
        if (element->type == UI_ELEMENT_BUTTON || element->type == UI_ELEMENT_LABEL)
        {
            static const char *const horizontal[] = {"L", "C", "R"};
            static const char *const vertical[] = {"T", "M", "B"};
            TextAlignmentRow(tool, UiLayoutNext(ui, &column, 0), "Text X",
                             &element->textAlignment.horizontal, horizontal);
            TextAlignmentRow(tool, UiLayoutNext(ui, &column, 0), "Text Y",
                             &element->textAlignment.vertical, vertical);
        }
        bool moved = NumberRow(ui, UiLayoutNext(ui, &column, 0), "X", &element->rect.x,
                               tool->entry[0], sizeof tool->entry[0], step, -4096, 4096);
        moved |= NumberRow(ui, UiLayoutNext(ui, &column, 0), "Y", &element->rect.y, tool->entry[1],
                           sizeof tool->entry[1], step, -4096, 4096);
        moved |= NumberRow(ui, UiLayoutNext(ui, &column, 0), "W", &element->rect.width,
                           tool->entry[2], sizeof tool->entry[2], step, 8, 4096);
        moved |= NumberRow(ui, UiLayoutNext(ui, &column, 0), "H", &element->rect.height,
                           tool->entry[3], sizeof tool->entry[3], step, 8, 4096);
        if (moved)
        {
            element->rect = UiClampRect(element->rect, bounds, 8);
            Say(tool, "%s at %d,%d  %dx%d", ElementName(element->type), element->rect.x,
                element->rect.y, element->rect.width, element->rect.height);
        }

        // Which edges of the containing region this element keeps its distance from.
        UiRect anchorRow = UiLayoutNext(ui, &column, 0);
        UiLabel(ui, (UiRect){anchorRow.x, anchorRow.y, ui->theme.itemHeight, anchorRow.height}, "A");
        UiRect anchorWell = {anchorRow.x + ui->theme.itemHeight, anchorRow.y,
                             anchorRow.width - ui->theme.itemHeight, anchorRow.height};
        UiDrawIndent(ui, anchorWell);
        UiRect anchorInner = UiRectInset(anchorWell, ui->theme.indentWidth);
        static const char *anchorNames[] = {"L", "R", "T", "B"};
        static const unsigned anchorBits[] = {UI_ANCHOR_LEFT, UI_ANCHOR_RIGHT, UI_ANCHOR_TOP,
                                              UI_ANCHOR_BOTTOM};
        for (int i = 0; i < 4; i++)
        {
            UiRect cell = UiLayoutCell(anchorInner, UI_LAYOUT_HORIZONTAL, i, 4, 0);
            UiButtonFlags flags = (UiButtonFlags)(UI_BUTTON_BARE | UI_BUTTON_TOGGLE_VISUAL);
            if (element->anchors & anchorBits[i])
                flags = (UiButtonFlags)(flags | UI_BUTTON_DOWN);
            if (UiButtonEx(ui, cell, anchorNames[i], flags))
            {
                element->anchors ^= anchorBits[i];
                Say(tool, "Anchors: %s%s%s%s",
                    element->anchors & UI_ANCHOR_LEFT ? "left " : "",
                    element->anchors & UI_ANCHOR_RIGHT ? "right " : "",
                    element->anchors & UI_ANCHOR_TOP ? "top " : "",
                    element->anchors & UI_ANCHOR_BOTTOM ? "bottom" : "");
            }
        }
        UiLabel(ui, UiLayoutNext(ui, &column, 0), "Least");
        NumberRow(ui, UiLayoutNext(ui, &column, 0), "w", &element->minWidth, tool->entry[4],
                  sizeof tool->entry[4], step, 0, 4096);
        NumberRow(ui, UiLayoutNext(ui, &column, 0), "h", &element->minHeight, tool->entry[5],
                  sizeof tool->entry[5], step, 0, 4096);
        UiLabel(ui, UiLayoutNext(ui, &column, 0), "Most (0 for no limit)");
        NumberRow(ui, UiLayoutNext(ui, &column, 0), "w", &element->maxWidth, tool->entry[8],
                  sizeof tool->entry[8], step, 0, 4096);
        NumberRow(ui, UiLayoutNext(ui, &column, 0), "h", &element->maxHeight, tool->entry[9],
                  sizeof tool->entry[9], step, 0, 4096);
        if (IsContainer(element->type))
        {
            static const char *flowNames[] = {"Shares: auto", "Shares: row", "Shares: column",
                                              "Shares: nothing"};
            if (UiButton(ui, UiLayoutNext(ui, &column, 0), flowNames[element->flow]))
            {
                element->flow = (UiFlow)((element->flow + 1) % UI_FLOW_COUNT);
                Say(tool, "%s divides its contents: %s", ElementName(element->type),
                    flowNames[element->flow]);
            }
        }
    }

    UiRect divider = UiLayoutNext(ui, &column, ui->theme.containerGap);
    DrawRectangle(divider.x, divider.y + 1, divider.width, 1, ui->theme.darkGrey);
    UiLabel(ui, UiLayoutNext(ui, &column, 0), "Surface");
    UiTextField(ui, UiLayoutNext(ui, &column, 0), tool->document.title,
                sizeof tool->document.title);
    if (UiButton(ui, UiLayoutNext(ui, &column, 0), StyleName(tool->document.style)))
    {
        tool->document.style =
            (UiSurfaceStyle)((tool->document.style + 1) % UI_SURFACE_STYLE_COUNT);
        Say(tool, "Surface style: %s", StyleName(tool->document.style));
    }
    int width = tool->document.surfaceWidth;
    int height = tool->document.surfaceHeight;
    bool resized = NumberRow(ui, UiLayoutNext(ui, &column, 0), "W", &width, tool->entry[6],
                             sizeof tool->entry[6], ui->theme.itemHeight / 2, 16, 4096);
    resized |= NumberRow(ui, UiLayoutNext(ui, &column, 0), "H", &height, tool->entry[7],
                         sizeof tool->entry[7], ui->theme.itemHeight / 2, 16, 4096);
    if (resized)
    {
        UiDocumentSetSurface(&tool->document, width, height, tool->document.style,
                             tool->document.title);
        Say(tool, "Surface %dx%d", tool->document.surfaceWidth, tool->document.surfaceHeight);
    }
}

static bool EnsureScratch(Tool *tool, size_t needed)
{
    if (needed <= tool->scratchCapacity)
        return true;
    UiRect *grown = realloc(tool->scratch, needed * sizeof(*grown));
    if (!grown)
        return false;
    tool->scratch = grown;
    tool->scratchCapacity = needed;
    return true;
}

static bool EnsureCanvas(Tool *tool, int width, int height)
{
    width = ClampInt(width, 1, 4096);
    height = ClampInt(height, 1, 4096);
    if (tool->canvas.texture.width == width && tool->canvas.texture.height == height)
        return true;
    if (tool->canvas.id)
        UnloadRenderTexture(tool->canvas);
    tool->canvas = LoadRenderTexture(width, height);
    if (!tool->canvas.id)
        return false;
    SetTextureFilter(tool->canvas.texture, TEXTURE_FILTER_POINT);
    return true;
}

static UiRect ToCanvas(const Tool *tool, UiRect rect)
{
    rect.x += tool->content.x;
    rect.y += tool->content.y;
    return rect;
}

static UiRect ToScreen(const Tool *tool, UiRect rect)
{
    UiRect canvas = ToCanvas(tool, rect);
    return (UiRect){(int)tool->origin.x + canvas.x * tool->zoom,
                    (int)tool->origin.y + canvas.y * tool->zoom, canvas.width * tool->zoom,
                    canvas.height * tool->zoom};
}

static int ElementAt(Tool *tool, Vector2 point)
{
    for (size_t i = tool->document.count; i-- > 0;)
        if (PointIn(ToCanvas(tool, tool->document.elements[i].rect), point))
            return (int)i;
    return -1;
}

static void EditSurface(Tool *tool)
{
    tool->guides = (UiSnapResult){0};
    if (tool->preview || Testing(tool) || UiMenuIsOpen(&tool->ui))
        return;
    Vector2 mouse = tool->pointer;
    bool inside = tool->pointerInWorkspace;
    bool dragging = tool->rectEditor.handle != UI_HANDLE_NONE;

    if (inside && !dragging && tool->input.mousePressed[MOUSE_BUTTON_LEFT])
    {
        if (tool->creating >= 0)
        {
            AddAt(tool, (UiElementType)tool->creating, (int)mouse.x - tool->content.x,
                  (int)mouse.y - tool->content.y);
            tool->creating = -1;
            return;
        }
        UiElement *current = Selected(tool);
        bool onHandle =
            current && UiHandleAt(&tool->ui, ToCanvas(tool, current->rect), mouse) != UI_HANDLE_NONE;
        if (!onHandle)
            tool->selected = ElementAt(tool, mouse);
    }

    // Panels are drawn over an oversized surface, so only the workspace drives edits.
    if (!inside && !dragging)
        return;
    UiElement *element = Selected(tool);
    if (!element)
        return;
    size_t others = 0;
    if (EnsureScratch(tool, tool->document.count ? tool->document.count : 1))
        for (size_t i = 0; i < tool->document.count; i++)
            if (i != (size_t)tool->selected)
                tool->scratch[others++] = ToCanvas(tool, tool->document.elements[i].rect);

    int gap = 0;
    UiRect bounds = ToCanvas(tool, ArrangeBounds(tool, element->rect, tool->selected, &gap));
    UiSnapConfig config = {bounds,
                           tool->scratch,
                           others,
                           tool->snap ? tool->gridStep : 0,
                           tool->snap ? SNAP_THRESHOLD : 0,
                           gap,
                           tool->ui.theme.itemHeight};
    UiRect rect = ToCanvas(tool, element->rect);
    UiHandle before = tool->rectEditor.handle;
    UiEditRect(&tool->ui, tool->content, &rect, &tool->rectEditor, &config, &tool->guides);
    if (before == UI_HANDLE_NONE && tool->rectEditor.handle != UI_HANDLE_NONE)
        PushUndo(tool);
    element->rect = (UiRect){rect.x - tool->content.x, rect.y - tool->content.y, rect.width,
                             rect.height};
    if (tool->rectEditor.handle != UI_HANDLE_NONE)
        Say(tool, "%s at %d,%d  %dx%d", ElementName(element->type), element->rect.x,
            element->rect.y, element->rect.width, element->rect.height);
}

static UiRect GripRect(const Tool *tool, UiRect outer)
{
    int size = tool->ui.theme.itemHeight / 2;
    return (UiRect){(int)tool->origin.x + outer.width * tool->zoom - size,
                    (int)tool->origin.y + outer.height * tool->zoom - size, size, size};
}

// Dragging the grip previews the layout at another size; it never touches the authored one.
static void ResizeSurface(Tool *tool, UiRect outer)
{
    // The drawn grip is small, so give the pointer a slightly wider corner to catch.
    UiRect grip = UiRectInset(GripRect(tool, outer), -4);
    if (!tool->preview && tool->input.mousePressed[MOUSE_BUTTON_LEFT] &&
        PointIn(grip, tool->input.mousePosition) && PointIn(tool->workspace, tool->input.mousePosition))
    {
        tool->resizing = true;
        tool->resizeOrigin = tool->origin;
    }
    if (!tool->input.mouseDown[MOUSE_BUTTON_LEFT])
        tool->resizing = false;
    if (!tool->resizing)
        return;
    int minimumWidth = 0;
    int minimumHeight = 0;
    UiDocumentMinimumSize(&tool->ui, &tool->document, &minimumWidth, &minimumHeight);
    UiRect content = UiDocumentContentRectSized(&tool->ui, &tool->document, tool->origin,
                                                TestWidth(tool), TestHeight(tool));
    int width = ((int)tool->input.mousePosition.x - content.x) / tool->zoom;
    int height = ((int)tool->input.mousePosition.y - content.y) / tool->zoom;
    tool->testWidth = ClampInt(width, minimumWidth, 4096);
    tool->testHeight = ClampInt(height, minimumHeight, 4096);
    Say(tool, "Testing %dx%d  (authored %dx%d, minimum %dx%d)", TestWidth(tool), TestHeight(tool),
        tool->document.surfaceWidth, tool->document.surfaceHeight, minimumWidth, minimumHeight);
}

static void DrawWorkspace(Tool *tool)
{
    const UiTheme *theme = &tool->ui.theme;
    // The blit is always the canvas at whole pixels: nothing here may stretch what was drawn.
    Texture texture = tool->canvas.texture;
    UiRect outer = {0, 0, texture.width, texture.height};
    DrawRectangle(tool->workspace.x, tool->workspace.y, tool->workspace.width,
                  tool->workspace.height, (Color){45, 48, 52, 255});

    // A screen-style layout is drawn straight onto the game image, so show it over a stand-in.
    // The checker stays screen-sized: it is a backdrop, not part of the design.
    if (tool->document.style == UI_SURFACE_SCREEN)
        for (int y = 0; y < outer.height * tool->zoom; y += 16)
            for (int x = 0; x < outer.width * tool->zoom; x += 16)
            {
                bool dark = ((x / 16) + (y / 16)) % 2 == 0;
                DrawRectangle((int)tool->origin.x + x, (int)tool->origin.y + y,
                              MinInt(16, outer.width * tool->zoom - x),
                              MinInt(16, outer.height * tool->zoom - y),
                              dark ? (Color){28, 30, 34, 255} : (Color){36, 39, 44, 255});
            }

    DrawTexturePro(texture, (Rectangle){0, 0, (float)texture.width, -(float)texture.height},
                   (Rectangle){tool->origin.x, tool->origin.y,
                               (float)(texture.width * tool->zoom),
                               (float)(texture.height * tool->zoom)},
                   (Vector2){0, 0}, 0, WHITE);

    UiElement *element = Selected(tool);
    if (element && !tool->preview && !Testing(tool))
        UiDrawRectSelection(&tool->ui, ToScreen(tool, element->rect));
    UiSnapResult guides = tool->guides;
    guides.vertical.position = (int)tool->origin.x + guides.vertical.position * tool->zoom;
    guides.vertical.start = (int)tool->origin.y + guides.vertical.start * tool->zoom;
    guides.vertical.end = (int)tool->origin.y + guides.vertical.end * tool->zoom;
    guides.horizontal.position = (int)tool->origin.y + guides.horizontal.position * tool->zoom;
    guides.horizontal.start = (int)tool->origin.x + guides.horizontal.start * tool->zoom;
    guides.horizontal.end = (int)tool->origin.x + guides.horizontal.end * tool->zoom;
    UiDrawGuides(&tool->ui, guides, (Color){80, 170, 255, 255});

    UiRect grip = GripRect(tool, outer);
    UiDrawOutset(&tool->ui, grip);
    for (int i = 2; i < grip.width - 1; i += 3)
        DrawRectangle(grip.x + i, grip.y + grip.height - 2, grip.width - i - 1, 1,
                      tool->ui.theme.black);

    char caption[160];
    if (Testing(tool))
        snprintf(caption, sizeof caption, "%s  TESTING %dx%d  (was %dx%d)",
                 tool->document.title[0] ? tool->document.title : "(untitled)", TestWidth(tool),
                 TestHeight(tool), tool->document.surfaceWidth, tool->document.surfaceHeight);
    else
        snprintf(caption, sizeof caption, "%s  %s  %dx%d  %dx zoom  %s",
                 tool->document.title[0] ? tool->document.title : "(untitled)",
                 StyleName(tool->document.style), tool->document.surfaceWidth,
                 tool->document.surfaceHeight, tool->zoom, tool->preview ? "PREVIEW" : "EDIT");
    UiDrawShadowText(&tool->ui, (int)tool->origin.x,
                     MaxInt(tool->workspace.y, (int)tool->origin.y - theme->itemHeight), caption,
                     theme->white);
}

// The surface is drawn once at true size; zoom is an integer blit of that image.
static void DrawCanvas(Tool *tool)
{
    const UiTheme *theme = &tool->ui.theme;
    ClearBackground(BLANK);
    UiDocumentDrawSurfaceSized(&tool->ui, &tool->document, (Vector2){0, 0}, TestWidth(tool),
                               TestHeight(tool));
    if (tool->grid && !tool->preview)
    {
        Color line = {(unsigned char)(theme->face.r - 20), (unsigned char)(theme->face.g - 20),
                      (unsigned char)(theme->face.b - 20), 255};
        if (tool->document.style == UI_SURFACE_SCREEN)
            line = (Color){60, 64, 70, 255};
        for (int x = theme->itemHeight; x < tool->content.width; x += theme->itemHeight)
            DrawRectangle(tool->content.x + x, tool->content.y, 1, tool->content.height, line);
        for (int y = theme->itemHeight; y < tool->content.height; y += theme->itemHeight)
            DrawRectangle(tool->content.x, tool->content.y + y, tool->content.width, 1, line);
    }
    EditSurface(tool);
    if (Testing(tool))
        UiDocumentDrawSized(&tool->ui, &tool->document, (Vector2){0, 0}, TestWidth(tool),
                            TestHeight(tool), tool->preview);
    else
        UiDocumentDrawElements(&tool->ui, &tool->document, (Vector2){0, 0}, tool->preview);
}

static void DrawStatus(Tool *tool, UiRect rect)
{
    UiDrawIndent(&tool->ui, rect);
    UiRect text = UiRectInset(rect, tool->ui.theme.indentWidth + 2);
    UiLabel(&tool->ui, text, tool->status);

    char right[128];
    UiElement *element = Selected(tool);
    if (tool->preview)
    {
        int hovered = ElementAt(tool, tool->pointer);
        snprintf(right, sizeof right, "PREVIEW  %s",
                 hovered >= 0 ? tool->document.elements[hovered].label : "hover a widget");
    }
    else if (Testing(tool))
        snprintf(right, sizeof right, "TESTING %dx%d   Escape returns to %dx%d", TestWidth(tool),
                 TestHeight(tool), tool->document.surfaceWidth, tool->document.surfaceHeight);
    else if (element)
        snprintf(right, sizeof right, "%s  %d,%d  %dx%d   snap %s  grid %d   %d elements",
                 ElementName(element->type), element->rect.x, element->rect.y, element->rect.width,
                 element->rect.height, tool->snap ? "on" : "off", tool->gridStep,
                 (int)tool->document.count);
    else
        snprintf(right, sizeof right, "snap %s  grid %d   %d elements", tool->snap ? "on" : "off",
                 tool->gridStep, (int)tool->document.count);
    int width = (int)ceilf(MeasureTextEx(tool->ui.theme.font, right, (float)tool->ui.theme.fontSize,
                                         (float)tool->ui.theme.textSpacing)
                               .x);
    UiLabel(&tool->ui, (UiRect){text.x + text.width - width, text.y, width, text.height}, right);
}

static void Nudge(Tool *tool, int dx, int dy, bool resize)
{
    UiElement *element = Selected(tool);
    if (!element)
        return;
    PushUndo(tool);
    if (resize)
    {
        element->rect.width += dx;
        element->rect.height += dy;
    }
    else
    {
        element->rect.x += dx;
        element->rect.y += dy;
    }
    element->rect = UiClampRect(element->rect, ContentBounds(tool), 8);
    Say(tool, "%s at %d,%d  %dx%d", ElementName(element->type), element->rect.x, element->rect.y,
        element->rect.width, element->rect.height);
}

static void Shortcuts(Tool *tool, const EngineInput *input)
{
    bool control = input->down[KEY_LEFT_CONTROL] || input->down[KEY_RIGHT_CONTROL];
    bool shift = input->down[KEY_LEFT_SHIFT] || input->down[KEY_RIGHT_SHIFT];
    if (control && input->pressed[KEY_Q])
        tool->quit = true;
    if (control && input->pressed[KEY_S])
        RunAction(tool, ACTION_SAVE);
    if (control && input->pressed[KEY_O])
        RunAction(tool, ACTION_LOAD);
    if (control && input->pressed[KEY_Z])
        RunAction(tool, ACTION_UNDO);
    if (control && input->pressed[KEY_D])
        RunAction(tool, ACTION_DUPLICATE);
    if (input->pressed[KEY_DELETE])
        RunAction(tool, ACTION_DELETE);
    if (input->pressed[KEY_PAGE_UP])
        RunAction(tool, ACTION_RAISE);
    if (input->pressed[KEY_PAGE_DOWN])
        RunAction(tool, ACTION_LOWER);
    if (!control)
    {
        if (input->pressed[KEY_P])
        {
            tool->preview = !tool->preview;
            Say(tool, tool->preview ? "Preview: widgets are live" : "Editing");
        }
        if (input->pressed[KEY_G])
            tool->grid = !tool->grid;
        if (input->pressed[KEY_S])
            tool->snap = !tool->snap;
        if (input->pressed[KEY_EQUAL] || input->pressed[KEY_KP_ADD])
            SetZoom(tool, tool->zoom + 1);
        if (input->pressed[KEY_MINUS] || input->pressed[KEY_KP_SUBTRACT])
            SetZoom(tool, tool->zoom - 1);
        if (input->wheel != 0 && PointIn(tool->workspace, input->mousePosition))
            SetZoom(tool, tool->zoom + (input->wheel > 0 ? 1 : -1));
        if (input->pressed[KEY_TAB] && tool->document.count)
        {
            tool->selected = (tool->selected + 1) % (int)tool->document.count;
            Say(tool, "Selected %d of %d", tool->selected + 1, (int)tool->document.count);
        }
    }
    int step = shift ? MaxInt(1, tool->gridStep) : 1;
    int dx = (input->pressed[KEY_RIGHT] - input->pressed[KEY_LEFT]) * step;
    int dy = (input->pressed[KEY_DOWN] - input->pressed[KEY_UP]) * step;
    if (dx || dy)
        Nudge(tool, dx, dy, control);
    if (input->pressed[KEY_ESCAPE])
    {
        if (UiMenuIsOpen(&tool->ui))
            UiCloseMenus(&tool->ui);
        else if (Testing(tool))
        {
            tool->testWidth = 0;
            tool->testHeight = 0;
            Say(tool, "Back to the authored size");
        }
        else if (tool->creating >= 0)
        {
            tool->creating = -1;
            Say(tool, "Placement cancelled");
        }
        else if (tool->preview)
        {
            tool->preview = false;
            Say(tool, "Editing");
        }
        else
            tool->selected = -1;
    }
}

static void Check(Tool *tool, bool condition, const char *message)
{
    TraceLog(condition ? LOG_INFO : LOG_ERROR, "CHECK %s: %s", message, condition ? "PASS" : "FAIL");
    if (!condition)
        tool->failures++;
}

static void SmokeChecks(Tool *tool)
{
    UiRect bounds = {0, 0, 96, 200};
    UiRect wide = UiClampRect((UiRect){20, 20, 140, 26}, bounds, 8);
    Check(tool, wide.width <= bounds.width && wide.x >= 0 && wide.x + wide.width <= bounds.width,
          "placement clamps an element wider than the surface");

    UiRect gridded = {17, 41, 60, 24};
    UiSnapConfig gridConfig = {bounds, NULL, 0, 6, 0, 3, 24};
    UiSnapRect(&gridded, UI_HANDLE_MOVE, gridConfig);
    Check(tool, gridded.x % 6 == 0 && gridded.y % 6 == 0, "grid snapping quantises a moved element");

    // Wide enough that the surface margins cannot be the nearer match.
    UiRect area = {0, 0, 200, 200};
    UiRect neighbour = {10, 10, 80, 24};
    UiRect aligned = {13, 60, 80, 24};
    UiSnapConfig alignConfig = {area, &neighbour, 1, 0, SNAP_THRESHOLD, 3, 24};
    UiSnapResult result = UiSnapRect(&aligned, UI_HANDLE_MOVE, alignConfig);
    Check(tool, aligned.x == neighbour.x && result.vertical.active,
          "a dragged element locks onto a neighbour's left edge and reports a guide");

    UiRect stacked = {10, 36, 80, 24};
    UiSnapResult stackResult = UiSnapRect(&stacked, UI_HANDLE_MOVE, alignConfig);
    Check(tool, stacked.y == neighbour.y + neighbour.height + 3 && stackResult.horizontal.active,
          "a dragged element locks onto the conventional gap below its neighbour");

    UiRect resized = {10, 10, 80, 26};
    UiSnapConfig sizeConfig = {bounds, NULL, 0, 0, SNAP_THRESHOLD, 3, 24};
    UiSnapRect(&resized, UI_HANDLE_BOTTOM, sizeConfig);
    Check(tool, resized.height == 24, "resizing settles on whole conventional rows");

    UiDocument round = {0};
    UiDocumentInit(&round, 260, 120, UI_SURFACE_WINDOW, "Round trip");
    UiElement *alignedButton = UiDocumentAdd(&round, UI_ELEMENT_BUTTON,
                                             (UiRect){4, 8, 100, 24}, "Restart");
    alignedButton->textAlignment = (UiTextAlignment){UI_ALIGN_END, UI_ALIGN_START};
    alignedButton->anchors = UI_ANCHOR_RIGHT;
    alignedButton->minWidth = 24;
    alignedButton->maxWidth = 100;
    bool saved = UiDocumentSave(&round, "build/core/ui_tool_roundtrip.ui");
    UiDocument reloaded = {0};
    bool loaded = saved && UiDocumentLoad(&reloaded, "build/core/ui_tool_roundtrip.ui");
    Check(tool,
          loaded && reloaded.surfaceWidth == 260 && reloaded.surfaceHeight == 120 &&
              reloaded.style == UI_SURFACE_WINDOW && !strcmp(reloaded.title, "Round trip") &&
              reloaded.count == 1 && !strcmp(reloaded.elements[0].label, "Restart") &&
              reloaded.elements[0].textAlignment.horizontal == UI_ALIGN_END &&
              reloaded.elements[0].textAlignment.vertical == UI_ALIGN_START &&
              reloaded.elements[0].anchors == UI_ANCHOR_RIGHT &&
              reloaded.elements[0].minWidth == 24 && reloaded.elements[0].maxWidth == 100,
          "documents round-trip text alignment alongside anchors, limits and surface settings");
    UiDocumentFree(&round);
    UiDocumentFree(&reloaded);

    // An indent is a container: contents are flush inside its bezel and arranged against it.
    size_t before = tool->document.count;
    UiElement *well =
        UiDocumentAdd(&tool->document, UI_ELEMENT_INDENT, (UiRect){40, 60, 120, 80}, "Well");
    UiRect inner = well ? ContainerContent(tool, well) : (UiRect){0};
    UiElement *inside = UiDocumentAdd(&tool->document, UI_ELEMENT_BUTTON,
                                      (UiRect){inner.x + 3, inner.y + 2, 60, 24}, "Inside");
    if (well && inside)
    {
        int containerGap = -1;
        int index = (int)tool->document.count - 1;
        UiRect containerBounds = ArrangeBounds(tool, inside->rect, index, &containerGap);
        Check(tool,
              containerBounds.x == inner.x && containerBounds.y == inner.y &&
                  containerBounds.width == inner.width && containerGap == 0,
              "an element inside an indent is arranged within the indent's bezel, without padding");

        UiRect flush = inside->rect;
        UiSnapConfig insideConfig = {containerBounds, NULL, 0, 0, SNAP_THRESHOLD, containerGap,
                                     tool->ui.theme.itemHeight};
        UiSnapRect(&flush, UI_HANDLE_MOVE, insideConfig);
        Check(tool, flush.x == inner.x && flush.y == inner.y,
              "snapping inside an indent locks flush to its bezel, not the surface edge");

        tool->selected = index;
        RunAction(tool, ACTION_ALIGN_RIGHT);
        Check(tool, tool->document.elements[index].rect.x == inner.x + inner.width - 60,
              "aligning right inside an indent uses the indent, not the surface");
        RunAction(tool, ACTION_FILL_WIDTH);
        Check(tool,
              tool->document.elements[index].rect.x == inner.x &&
                  tool->document.elements[index].rect.width == inner.width,
              "filling the width inside an indent spans its bezel exactly");
        Check(tool, UiDocumentInsideIndent(&tool->document, (size_t)index) &&
                        !UiDocumentInsideIndent(&tool->document, 0),
              "a control in a placed indent is drawn flush, without nesting a second one");
    }
    while (tool->document.count > before)
        UiDocumentRemove(&tool->document, tool->document.count - 1);
    ClearUndo(tool);
    tool->selected = 0;

    // Resizing: anchors decide who moves, who stretches and who floats.
    UiDocument sized = {0};
    UiDocumentInit(&sized, 200, 100, UI_SURFACE_PANEL, "Resize");
    UiElement *pinned = UiDocumentAdd(&sized, UI_ELEMENT_BUTTON, (UiRect){10, 10, 60, 24}, "Left");
    UiElement *stretched =
        UiDocumentAdd(&sized, UI_ELEMENT_BUTTON, (UiRect){10, 40, 180, 24}, "Wide");
    UiElement *trailing =
        UiDocumentAdd(&sized, UI_ELEMENT_BUTTON, (UiRect){140, 70, 50, 24}, "Right");
    if (pinned && stretched && trailing)
    {
        Check(tool, pinned->anchors == (UI_ANCHOR_LEFT | UI_ANCHOR_TOP),
              "a plain control keeps the size it was drawn at when the surface changes");
        stretched->anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP;
        trailing->anchors = UI_ANCHOR_RIGHT | UI_ANCHOR_TOP;
        const UiRect *rects = UiDocumentResolve(&tool->ui, &sized, 300, 100);
        Check(tool, rects && rects[0].x == 10 && rects[0].width == 60,
              "a left-anchored element keeps its place when the surface grows");
        Check(tool, rects && rects[1].width == 280,
              "an element pinned to both sides stretches by the full change");
        Check(tool, rects && rects[2].x == 240 && rects[2].width == 50,
              "a right-anchored element travels with the edge at its original width");
        Check(tool, sized.elements[1].rect.width == 180,
              "resolving never edits the authored rectangles");

        UiElement *well = UiDocumentAdd(&sized, UI_ELEMENT_INDENT, (UiRect){0, 0, 200, 30}, "Well");
        if (well)
        {
            well->anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP;
            well->flow = UI_FLOW_ROW;
            sized.elements[0].rect = (UiRect){1, 1, 66, 28};
            UiDocumentAdd(&sized, UI_ELEMENT_BUTTON, (UiRect){67, 1, 66, 28}, "Two");
            UiDocumentAdd(&sized, UI_ELEMENT_BUTTON, (UiRect){133, 1, 66, 28}, "Three");
            rects = UiDocumentResolve(&tool->ui, &sized, 301, 100);
            UiRect inner = UiRectInset(rects[3], tool->ui.theme.indentWidth);
            bool flush = rects[0].x == inner.x &&
                         rects[4].x == rects[0].x + rects[0].width &&
                         rects[5].x == rects[4].x + rects[4].width &&
                         rects[5].x + rects[5].width == inner.x + inner.width;
            int widest = MaxInt(rects[0].width, MaxInt(rects[4].width, rects[5].width));
            int narrowest = MinInt(rects[0].width, MinInt(rects[4].width, rects[5].width));
            Check(tool, flush, "a row flow divides its indent with no gaps and no overhang");
            Check(tool, widest - narrowest <= 1,
                  "an odd remainder is spread, so the cells differ by at most a pixel");
        }
        int minimumWidth = 0;
        int minimumHeight = 0;
        UiDocumentMinimumSize(&tool->ui, &sized, &minimumWidth, &minimumHeight);
        Check(tool, minimumWidth >= 3 * UI_ELEMENT_MINIMUM_SIZE,
              "the minimum size accounts for what the contents need");

        // Shrinking must stop at what the label needs, not squeeze a control down to nothing.
        UiDocument crush = {0};
        UiDocumentInit(&crush, 300, 100, UI_SURFACE_PANEL, "Crush");
        UiElement *wide = UiDocumentAdd(&crush, UI_ELEMENT_BUTTON, (UiRect){10, 10, 260, 24},
                                        "A button with a long label");
        UiElement *tight = UiDocumentAdd(&crush, UI_ELEMENT_BUTTON, (UiRect){10, 40, 40, 24}, "Ok");
        if (wide && tight)
        {
            wide->anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP;
            LabelMinimum(tool, wide, &wide->minWidth, &wide->minHeight);
            LabelMinimum(tool, tight, &tight->minWidth, &tight->minHeight);
            const UiRect *tiny = UiDocumentResolve(&tool->ui, &crush, 40, 100);
            int label = (int)ceilf(MeasureTextEx(tool->ui.theme.font, wide->label,
                                                 (float)tool->ui.theme.fontSize,
                                                 (float)tool->ui.theme.textSpacing)
                                       .x);
            Check(tool, tiny && tiny[0].width >= label,
                  "a stretched control refuses to shrink below the width of its own label");
            Check(tool, tiny && tiny[1].width == 40,
                  "a control that was drawn tight is left alone, not crushed further");
            int floorWidth = 0;
            UiDocumentMinimumSize(&tool->ui, &crush, &floorWidth, NULL);
            Check(tool, floorWidth >= label + 10,
                  "the surface reports a minimum that keeps every control readable");
        }
        UiDocumentFree(&crush);
    }
    UiDocumentFree(&sized);

    // Shared indents must enforce limits on both axes, including a max below the label minimum.
    for (int axis = 0; axis < 2; axis++)
    {
        bool row = axis == 0;
        UiDocument capped = {0};
        UiDocumentInit(&capped, 200, 200, UI_SURFACE_PANEL, "Capped flow");
        UiElement *region = UiDocumentAdd(&capped, UI_ELEMENT_INDENT,
                                           (UiRect){0, 0, 200, 200}, "Well");
        region->anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP | UI_ANCHOR_BOTTOM;
        region->flow = row ? UI_FLOW_ROW : UI_FLOW_COLUMN;
        for (int i = 0; i < 3; i++)
            UiDocumentAdd(&capped, UI_ELEMENT_BUTTON,
                          row ? (UiRect){1 + 66 * i, 1, 66, 198}
                              : (UiRect){1, 1 + 66 * i, 198, 66}, "Button");
        capped.elements[1].minWidth = row ? 40 : 8;
        capped.elements[1].minHeight = row ? 8 : 40;
        capped.elements[1].maxWidth = row ? 24 : 12;
        capped.elements[1].maxHeight = row ? 12 : 24;
        const UiRect *rects = UiDocumentResolve(&tool->ui, &capped, 301, 301);
        UiRect inner = UiRectInset(rects[0], tool->ui.theme.indentWidth);
        int extent = row ? inner.width : inner.height;
        int first = row ? rects[1].width : rects[1].height;
        int second = row ? rects[2].width : rects[2].height;
        int third = row ? rects[3].width : rects[3].height;
        Check(tool, first == 24 && (row ? rects[1].height : rects[1].width) == 12 &&
                        first + second + third == extent && abs(second - third) <= 1 &&
                        (row ? rects[2].x == rects[1].x + first &&
                                   rects[3].x == rects[2].x + second
                             : rects[2].y == rects[1].y + first &&
                                   rects[3].y == rects[2].y + second),
              row ? "row flow respects a 24px maximum and shares the surplus without gaps"
                  : "column flow respects maximums on both axes and shares the surplus");
        capped.elements[2].maxWidth = row ? 30 : 0;
        capped.elements[2].maxHeight = row ? 0 : 30;
        capped.elements[3].maxWidth = row ? 18 : 0;
        capped.elements[3].maxHeight = row ? 0 : 18;
        rects = UiDocumentResolve(&tool->ui, &capped, 401, 401);
        Check(tool, (row ? rects[1].width == 24 && rects[2].width == 30 && rects[3].width == 18
                         : rects[1].height == 24 && rects[2].height == 30 && rects[3].height == 18),
              "when every flow child is capped, surplus space never stretches them past their limits");
        int minimumWidth, minimumHeight;
        UiDocumentMinimumSize(&tool->ui, &capped, &minimumWidth, &minimumHeight);
        Check(tool, (row ? minimumWidth : minimumHeight) == 24 + 8 + 8 +
                                                                  2 * tool->ui.theme.indentWidth,
              "surface minimums honor an explicit maximum below a child's minimum");
        UiDocumentFree(&capped);
    }

    // Two panels side by side, both told to stretch: they must divide the change, not each take it.
    UiDocument split = {0};
    UiDocumentInit(&split, 320, 200, UI_SURFACE_WINDOW, "Split");
    UiElement *leftPane = UiDocumentAdd(&split, UI_ELEMENT_INDENT, (UiRect){3, 3, 152, 194}, "Left");
    UiElement *rightPane =
        UiDocumentAdd(&split, UI_ELEMENT_INDENT, (UiRect){161, 3, 156, 194}, "Right");
    if (leftPane && rightPane)
    {
        unsigned fill = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP | UI_ANCHOR_BOTTOM;
        leftPane->anchors = fill;
        rightPane->anchors = fill;
        int authoredGap = rightPane->rect.x - (leftPane->rect.x + leftPane->rect.width);
        const UiRect *rects = UiDocumentResolve(&tool->ui, &split, 420, 260);
        int gap = rects[1].x - (rects[0].x + rects[0].width);
        Check(tool, rects[0].x + rects[0].width <= rects[1].x,
              "two panels that both stretch never run into each other");
        Check(tool, gap == authoredGap, "the gap between them stays exactly as it was drawn");
        Check(tool, rects[0].width > leftPane->rect.width &&
                        rects[1].width > rightPane->rect.width &&
                        rects[1].x + rects[1].width == 420 - 3,
              "both grow, and together they still reach the far margin");
        // They are beside each other, not stacked, so neither halves the height with the other.
        Check(tool, rects[0].y == rects[1].y && rects[0].height == rects[1].height &&
                        rects[0].height > leftPane->rect.height,
              "side by side, they each take the whole height instead of dividing it");
    }
    UiDocumentFree(&split);

    // The same thing downwards: stacked panels divide the height and each keep the full width.
    UiDocument stack = {0};
    UiDocumentInit(&stack, 320, 200, UI_SURFACE_WINDOW, "Stack");
    UiElement *upper = UiDocumentAdd(&stack, UI_ELEMENT_INDENT, (UiRect){3, 3, 314, 94}, "Upper");
    UiElement *lower = UiDocumentAdd(&stack, UI_ELEMENT_INDENT, (UiRect){3, 103, 314, 94}, "Lower");
    if (upper && lower)
    {
        Check(tool, upper->anchors == (UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP |
                                       UI_ANCHOR_BOTTOM),
              "a new container fills the region it is given, both ways");
        const UiRect *rects = UiDocumentResolve(&tool->ui, &stack, 420, 300);
        int gap = rects[1].y - (rects[0].y + rects[0].height);
        Check(tool, rects[0].y + rects[0].height <= rects[1].y && gap == 6,
              "stacked panels divide the height and keep the gap between them");
        Check(tool, rects[0].height > upper->rect.height && rects[1].height > lower->rect.height &&
                        rects[1].y + rects[1].height == 300 - 3,
              "both grow downwards and together they reach the bottom margin");
        Check(tool, rects[0].x == rects[1].x && rects[0].width == rects[1].width &&
                        rects[0].width == 414,
              "stacked, they each take the whole width instead of dividing it");
    }
    UiDocumentFree(&stack);

    // A capped panel stops at its limit and the others take what it left, so the margins still fit.
    UiDocument capped = {0};
    UiDocumentInit(&capped, 320, 200, UI_SURFACE_WINDOW, "Capped");
    UiElement *first = UiDocumentAdd(&capped, UI_ELEMENT_INDENT, (UiRect){3, 3, 100, 194}, "A");
    UiElement *middle = UiDocumentAdd(&capped, UI_ELEMENT_INDENT, (UiRect){109, 3, 100, 194}, "B");
    UiElement *last = UiDocumentAdd(&capped, UI_ELEMENT_INDENT, (UiRect){215, 3, 102, 194}, "C");
    if (first && middle && last)
    {
        Check(tool, middle->maxWidth == 0 && middle->maxHeight == 0,
              "an element has no limit until one is set");
        middle->maxWidth = 120;
        const UiRect *rects = UiDocumentResolve(&tool->ui, &capped, 500, 200);
        Check(tool, rects && rects[1].width == 120,
              "a capped panel stops growing exactly at its limit");
        Check(tool, rects && rects[1].x - (rects[0].x + rects[0].width) == 6 &&
                        rects[2].x - (rects[1].x + rects[1].width) == 6,
              "the gaps either side of it stay as they were drawn");
        Check(tool, rects && rects[2].x + rects[2].width == 500 - 3,
              "the others take what it left, so the row still reaches the margin");
        Check(tool, rects && rects[0].width > 100 && rects[2].width > 102,
              "the surplus goes to the panels that can still grow");
        middle->maxHeight = 150;
        rects = UiDocumentResolve(&tool->ui, &capped, 500, 400);
        Check(tool, rects && rects[1].height == 150 && rects[0].height > 194,
              "a height limit holds while its neighbours keep filling");
    }
    UiDocumentFree(&capped);

    // Four by four: each cell divides width with its row and height with its column, nothing else.
    UiDocument grid = {0};
    UiDocumentInit(&grid, 320, 200, UI_SURFACE_WINDOW, "Grid");
    for (int row = 0; row < 4; row++)
        for (int column = 0; column < 4; column++)
            UiDocumentAdd(&grid, UI_ELEMENT_INDENT,
                          (UiRect){3 + column * 79, 3 + row * 49, 75, 45}, "Cell");
    const UiRect *cells = UiDocumentResolve(&tool->ui, &grid, 640, 400);
    bool tidy = cells != NULL;
    for (int row = 0; cells && row < 4; row++)
        for (int column = 0; column < 4; column++)
        {
            UiRect cell = cells[row * 4 + column];
            if (column && cell.x < cells[row * 4 + column - 1].x + cells[row * 4 + column - 1].width)
                tidy = false; // ran into the cell on its left
            if (row && cell.y < cells[(row - 1) * 4 + column].y + cells[(row - 1) * 4 + column].height)
                tidy = false; // ran into the cell above
            if (cell.y != cells[row * 4].y || cell.height != cells[row * 4].height)
                tidy = false; // a row must stay level
            if (cell.x != cells[column].x || cell.width != cells[column].width)
                tidy = false; // a column must stay in line
        }
    Check(tool, tidy, "a four by four grid stays a grid: rows level, columns in line, no overlap");
    // The authored margins are kept, so the far corner sits exactly as far in as it was drawn.
    int rightMargin = 320 - (3 + 3 * 79 + 75);
    int bottomMargin = 200 - (3 + 3 * 49 + 45);
    Check(tool, cells && cells[15].x + cells[15].width == 640 - rightMargin &&
                    cells[15].y + cells[15].height == 400 - bottomMargin,
          "the grid still reaches the far corner, keeping the margin it was drawn with");
    Check(tool, cells && cells[0].width > 75 && cells[0].height > 45,
          "every cell in the grid took a share of the growth");
    UiDocumentFree(&grid);

    // A player window: a viewport that fills, and a bar of transport buttons along the bottom.
    UiDocument player = {0};
    UiDocumentInit(&player, 320, 200, UI_SURFACE_WINDOW, "Video Player");
    UiElement *viewport = UiDocumentAdd(&player, UI_ELEMENT_INDENT, (UiRect){3, 3, 314, 157}, "View");
    UiElement *bar = UiDocumentAdd(&player, UI_ELEMENT_INDENT, (UiRect){3, 163, 314, 26}, "Bar");
    static const char *transport[] = {"Play", "Pause", "Back", "Fwd"};
    if (viewport && bar)
    {
        viewport->anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP | UI_ANCHOR_BOTTOM;
        bar->anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_BOTTOM;
        for (int i = 0; i < 4; i++)
        {
            UiElement *button = UiDocumentAdd(&player, UI_ELEMENT_BUTTON,
                                              (UiRect){4 + i * 78, 164, 78, 24}, transport[i]);
            LabelMinimum(tool, button, &button->minWidth, &button->minHeight);
        }
        Check(tool, bar->flow == UI_FLOW_AUTO,
              "an indent shares itself among its contents unless told otherwise");

        int widths[2] = {260, 410};
        for (int pass = 0; pass < 2; pass++)
        {
            const UiRect *rects = UiDocumentResolve(&tool->ui, &player, widths[pass], 200);
            bool clean = rects != NULL;
            for (int i = 0; rects && i < 4; i++)
            {
                UiRect button = rects[2 + i];
                int natural = ButtonWidth(tool, transport[i]);
                if (button.width < MinInt(natural, 78) || button.height != 24)
                    clean = false;
                if (i && button.x < rects[1 + i].x + rects[1 + i].width)
                    clean = false; // overlapping a neighbour
            }
            Check(tool, clean,
                  pass == 0 ? "squishing the window never overlaps or crushes the button row"
                            : "stretching the window keeps the buttons flush and side by side");
            // The bar is shared out: the buttons grow together and still meet exactly.
            UiRect inner = UiRectInset(rects[1], tool->ui.theme.indentWidth);
            int widest = 0;
            int narrowest = 4096;
            for (int i = 0; i < 4; i++)
            {
                widest = MaxInt(widest, rects[2 + i].width);
                narrowest = MinInt(narrowest, rects[2 + i].width);
            }
            Check(tool,
                  rects[2].x == inner.x &&
                      rects[5].x + rects[5].width == inner.x + inner.width && widest - narrowest <= 1,
                  pass == 0 ? "the squished bar is still divided evenly end to end"
                            : "the stretched bar is divided evenly end to end, with no gap left over");
        }
        const UiRect *rects = UiDocumentResolve(&tool->ui, &player, 410, 260);
        Check(tool, rects && rects[0].height > viewport->rect.height &&
                        rects[1].y > bar->rect.y && rects[1].height == bar->rect.height,
              "the viewport takes the new height and the bar rides the bottom edge");
        int floorWidth = 0;
        UiDocumentMinimumSize(&tool->ui, &player, &floorWidth, NULL);
        int needed = 0;
        int ignored = 0;
        LabelMinimum(tool, &player.elements[3], &needed, &ignored); // the widest label, "Pause"
        int sum = 0;
        for (int i = 0; i < 4; i++)
            sum += player.elements[2 + i].minWidth;
        int chrome = 6 + 2 * tool->ui.theme.indentWidth;
        Check(tool, floorWidth == sum + chrome && floorWidth < 4 * needed + chrome,
              "the row minimum sums individual button minimums rather than repeating the widest");
        bool continuous = true;
        for (int w = floorWidth; w <= 4 * needed + chrome; w++)
        {
            rects = UiDocumentResolve(&tool->ui, &player, w, 200);
            UiRect inside = UiRectInset(rects[1], tool->ui.theme.indentWidth);
            int edge = inside.x;
            for (int i = 0; i < 4; i++)
            {
                UiRect button = rects[2 + i];
                continuous &= button.x == edge && button.width >= player.elements[2 + i].minWidth;
                if (w == floorWidth)
                    continuous &= button.width == player.elements[2 + i].minWidth;
                edge += button.width;
            }
            continuous &= edge == inside.x + inside.width;
        }
        Check(tool, continuous,
              "shorter buttons keep shrinking after the widest stops, without gaps or overlap");
        rects = UiDocumentResolve(&tool->ui, &player, floorWidth, 200);
        UiRect last = rects[5];
        UiRect wellInside = UiRectInset(rects[1], tool->ui.theme.indentWidth);
        Check(tool, last.x + last.width <= wellInside.x + wellInside.width,
              "at that minimum the last button still fits inside its bar");
        for (int i = 0; i < 4; i++)
        {
            int label = 0;
            LabelMinimum(tool, &player.elements[2 + i], &label, &ignored);
            if (rects[2 + i].width < label)
                needed = -1;
        }
        Check(tool, needed > 0, "at that minimum every button still shows its whole label");
    }
    UiDocumentFree(&player);

    FILE *legacy = fopen("build/core/ui_tool_v1.ui", "wb");
    if (legacy)
    {
        fprintf(legacy, "core_ui_document 1\n0 32 32 140 26 0.5 0\tButton\n");
        fclose(legacy);
        UiDocument old = {0};
        bool ok = UiDocumentLoad(&old, "build/core/ui_tool_v1.ui");
        Check(tool, ok && old.count == 1 && old.surfaceWidth > 0,
              "version 1 layouts still load and receive a default surface");
        UiDocumentFree(&old);
    }
}

// Scripted pointer input, so the fixed bugs stay fixed without a human at the mouse.
static void SmokeFrame(Tool *tool)
{
    EngineInput *input = &tool->input;
    *input = (EngineInput){0};
    int frame = tool->frames;
    // The panel's title bar: chrome that owns no widget, so only the routing fix decides the outcome.
    Vector2 palettePoint = {(float)(tool->palette.rect.x + tool->palette.rect.width / 2),
                            (float)(tool->palette.rect.y + tool->ui.theme.titleHeight / 2)};
    // Wherever the window manager put the workspace, click where surface and workspace overlap.
    UiRect shown = ToScreen(tool, (UiRect){0, 0, tool->document.surfaceWidth,
                                           tool->document.surfaceHeight});
    Vector2 surfacePoint = {(float)(MaxInt(shown.x, tool->workspace.x) + 12),
                            (float)(MaxInt(shown.y, tool->workspace.y) + 12)};

    if (frame == 3)
    {
        tool->creating = UI_ELEMENT_BUTTON;
        tool->selected = 0;
        input->mousePosition = palettePoint;
        input->mousePressed[MOUSE_BUTTON_LEFT] = true;
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 4)
    {
        input->mousePosition = palettePoint;
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 5)
    {
        Check(tool, tool->document.count == 2 && tool->creating == UI_ELEMENT_BUTTON,
              "a click on the palette never leaks onto the surface");
        Check(tool, tool->selected == 0, "chrome clicks keep the current selection");
        input->mousePosition = surfacePoint;
        input->mousePressed[MOUSE_BUTTON_LEFT] = true;
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 6)
    {
        input->mousePosition = surfacePoint;
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 7)
    {
        Check(tool, tool->document.count == 3 && tool->creating < 0,
              "a click on the surface places the armed element");
        tool->selected = 2;
    }
    else if (frame >= 10 && frame <= 16)
    {
        // Drag the new element up towards the first one so a guide has to appear.
        UiElement *element = &tool->document.elements[2];
        UiRect onScreen = ToScreen(tool, element->rect);
        Vector2 grab = {(float)(onScreen.x + onScreen.width / 2),
                        (float)(onScreen.y + onScreen.height / 2)};
        input->mousePosition = grab;
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
        if (frame == 10)
            input->mousePressed[MOUSE_BUTTON_LEFT] = true;
        else
        {
            UiRect target = tool->document.elements[0].rect;
            int stepX = target.x - element->rect.x;
            int stepY = target.y + target.height + 3 - element->rect.y;
            input->mousePosition.x += (float)ClampInt(stepX, -18, 18);
            input->mousePosition.y += (float)ClampInt(stepY, -18, 18);
        }
    }
    else if (frame == 17)
    {
        UiElement *element = &tool->document.elements[2];
        UiRect first = tool->document.elements[0].rect;
        Check(tool, element->rect.x == first.x,
              "dragging locks the element to its neighbour's left edge");
        Check(tool, element->rect.y == first.y + first.height + tool->ui.theme.containerGap,
              "dragging locks the conventional gap under the neighbour");
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 19)
    {
        RunAction(tool, ACTION_ALIGN_CENTRE);
        UiElement *element = &tool->document.elements[2];
        Check(tool,
              element->rect.x ==
                  (tool->document.surfaceWidth - element->rect.width) / 2,
              "centre alignment uses the surface, not the window");
        RunAction(tool, ACTION_UNDO);
        Check(tool, tool->document.elements[2].rect.x == tool->document.elements[0].rect.x,
              "undo restores the previous geometry");
    }
    else if (frame == 20)
    {
        // Renaming was impossible before: the click that focused the field cleared the selection.
        input->mousePosition = (Vector2){(float)(tool->labelField.x + tool->labelField.width / 2),
                                         (float)(tool->labelField.y + tool->labelField.height / 2)};
        input->mousePressed[MOUSE_BUTTON_LEFT] = true;
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 21)
    {
        Check(tool, tool->selected == 2, "clicking the label field keeps the element selected");
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
        input->text[0] = '!';
        input->textCount = 1;
    }
    else if (frame == 22)
    {
        UiElement *element = &tool->document.elements[2];
        size_t length = strlen(element->label);
        Check(tool, length > 0 && element->label[length - 1] == '!',
              "typing into the label field renames the selected element");
        Check(tool, UiTextFieldActive(&tool->ui), "the focused field owns the keyboard");
        Check(tool, UiTextFieldFocused(&tool->ui, element->label),
              "the field that was clicked is the one holding focus");
        tool->preview = true;
    }
    else if (frame == 24)
    {
        tool->preview = false;
        tool->document.style = UI_SURFACE_SCREEN;
        SetZoom(tool, 2);
    }
    else if (frame == 25)
    {
        // Zoomed in, the surface overflows the workspace: pan it back into view.
        input->mousePosition = (Vector2){(float)(tool->workspace.x + tool->workspace.width / 2),
                                         (float)(tool->workspace.y + tool->workspace.height / 2)};
        input->mousePressed[MOUSE_BUTTON_MIDDLE] = true;
        input->mouseDown[MOUSE_BUTTON_MIDDLE] = true;
    }
    else if (frame == 26)
    {
        input->mousePosition = (Vector2){(float)(tool->workspace.x + tool->workspace.width / 2),
                                         (float)(tool->workspace.y + tool->workspace.height / 2)};
        input->mouseDown[MOUSE_BUTTON_MIDDLE] = true;
        input->mouseDelta =
            (Vector2){(float)(tool->workspace.x + 4 - (int)tool->origin.x),
                      (float)(tool->workspace.y + tool->ui.theme.itemHeight + 4 -
                              (int)tool->origin.y)};
    }
    else if (frame == 27)
    {
        Check(tool, (int)tool->origin.x == tool->workspace.x + 4,
              "dragging with the middle button pans the view");
        input->mouseReleased[MOUSE_BUTTON_MIDDLE] = true;
        // Near its corner, not its centre: at zoom the centre can sit outside a narrow workspace.
        UiRect first = ToScreen(tool, tool->document.elements[0].rect);
        input->mousePosition = (Vector2){(float)(first.x + 6), (float)(first.y + 6)};
        input->mousePressed[MOUSE_BUTTON_LEFT] = true;
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 28)
    {
        Check(tool, tool->selected == 0,
              "hit testing maps the pointer back through the zoom and the pan");
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 30)
    {
        // Stage the button and the well where this workspace can see them, so the drag is real.
        SetZoom(tool, 1);
        int docX = (int)(surfacePoint.x - tool->origin.x) - tool->content.x;
        int docY = (int)(surfacePoint.y - tool->origin.y) - tool->content.y;
        tool->document.elements[2].rect = (UiRect){docX, docY, 60, 24};
        UiDocumentAdd(&tool->document, UI_ELEMENT_INDENT,
                      (UiRect){docX, docY + 40, 100, 60}, "Well");
        tool->selected = 2;
    }
    else if (frame >= 32 && frame <= 40)
    {
        // Drag the button into the indent; it should take the bezel, not the surface edge.
        UiElement *element = &tool->document.elements[2];
        UiRect inner = ContainerContent(tool, &tool->document.elements[3]);
        UiRect onScreen = ToScreen(tool, element->rect);
        input->mousePosition = (Vector2){(float)(onScreen.x + onScreen.width / 2),
                                         (float)(onScreen.y + onScreen.height / 2)};
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
        if (frame == 32)
            input->mousePressed[MOUSE_BUTTON_LEFT] = true;
        else
        {
            input->mousePosition.x +=
                (float)ClampInt(inner.x + 2 - element->rect.x, -48, 48) * tool->zoom;
            input->mousePosition.y +=
                (float)ClampInt(inner.y + 2 - element->rect.y, -48, 48) * tool->zoom;
        }
    }
    else if (frame == 42)
    {
        tool->document.style = UI_SURFACE_WINDOW; // so the capture shows a real window frame
        // Park the corner inside the workspace, whatever size the window manager gave us.
        UiRect outer = UiDocumentOuterRectSized(&tool->ui, &tool->document, (Vector2){0, 0},
                                                TestWidth(tool), TestHeight(tool));
        UiRect grip = GripRect(tool, outer);
        tool->pan.x += (float)(tool->workspace.x + tool->workspace.width - 24 - grip.x);
        tool->pan.y += (float)(tool->workspace.y + tool->workspace.height - 24 - grip.y);
    }
    else if (frame >= 44 && frame <= 47)
    {
        // Drag the grip: the layout must respond without the authored rects changing.
        if (frame == 44)
        {
            // The button sits in the indent, so both must stretch for the chain to reach it.
            tool->document.elements[2].anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP;
            tool->document.elements[3].anchors = UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP;
            tool->selected = 2;
        }
        // Measured from the authored corner, which stays put while the corner is held.
        UiRect authored = UiDocumentOuterRectSized(&tool->ui, &tool->document, (Vector2){0, 0},
                                                   tool->document.surfaceWidth,
                                                   tool->document.surfaceHeight);
        UiRect grip = GripRect(tool, authored);
        input->mousePosition = (Vector2){(float)(grip.x + grip.width / 2 + (frame - 44) * 16),
                                         (float)(grip.y + grip.height / 2)};
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
        if (frame == 44)
            input->mousePressed[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 48)
    {
        UiRect authored = tool->document.elements[2].rect;
        Check(tool, Testing(tool) && TestWidth(tool) > tool->document.surfaceWidth,
              "dragging the grip tests the layout at a larger surface");
        const UiRect *rects = UiDocumentResolve(&tool->ui, &tool->document, TestWidth(tool),
                                                TestHeight(tool));
        Check(tool, rects && rects[2].width > authored.width &&
                        tool->document.elements[2].rect.width == authored.width,
              "a stretched element grows on screen while its authored width is untouched");
        UiRect outer = UiDocumentOuterRectSized(&tool->ui, &tool->document, (Vector2){0, 0},
                                                TestWidth(tool), TestHeight(tool));
        // If the canvas ever differed from the surface, the blit would squash the pixels in it.
        Check(tool, tool->canvas.texture.width == outer.width &&
                        tool->canvas.texture.height == outer.height,
              "the canvas matches the tested surface, so nothing is ever scaled to fit");
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 50)
    {
        tool->testWidth = 0;
        tool->testHeight = 0;
        Check(tool, !Testing(tool), "clearing the test size returns to the authored layout");
        tool->selected = 2;
    }
    else if (frame == 52)
    {
        input->mousePosition = (Vector2){(float)(tool->labelField.x + tool->labelField.width / 2),
                                         (float)(tool->labelField.y + tool->labelField.height / 2)};
        input->mousePressed[MOUSE_BUTTON_LEFT] = true;
        input->mouseDown[MOUSE_BUTTON_LEFT] = true;
    }
    else if (frame == 53)
    {
        // Select all, cut, paste back: the clipboard round-trip the boxes rely on.
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
        input->down[KEY_LEFT_CONTROL] = true;
        input->pressed[KEY_A] = true;
    }
    else if (frame == 54)
    {
        input->down[KEY_LEFT_CONTROL] = true;
        input->pressed[KEY_X] = true;
    }
    else if (frame == 55)
    {
        Check(tool, tool->document.elements[2].label[0] == '\0',
              "select all then cut empties the field");
        // Something outside ASCII, to see how far it gets.
        SetClipboardText("caf\xc3\xa9 \xe2\x86\x92 \xce\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80 \xe2\x9d\xa4\xef\xb8\x8f");
        input->down[KEY_LEFT_CONTROL] = true;
        input->pressed[KEY_V] = true;
    }
    else if (frame == 56)
    {
        const char *pasted = tool->document.elements[2].label;
        Check(tool, !strcmp(pasted, "caf\xc3\xa9 \xe2\x86\x92 \xce\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80 \xe2\x9d\xa4\xef\xb8\x8f"),
              "paste keeps every byte, including characters outside ASCII");
        // Pasting expands the atlas; check actual glyphs as well as preserved UTF-8 bytes.
        Font font = tool->ui.theme.font;
        Check(tool, font.glyphs[GetGlyphIndex(font, 'A')].offsetY == 0 &&
                        font.glyphs[GetGlyphIndex(font, '?')].offsetY == 0,
              "pasting a missing emoji leaves ordinary text and fallback glyphs aligned");
        unsigned atlas = font.texture.id;
        UiTextWidth(&tool->ui, "\xf0\x9f\x98\x80");
        Check(tool, tool->ui.theme.font.texture.id == atlas &&
                        font.glyphs[GetGlyphIndex(font, 0x1f600)].value == '?',
              "an unavailable emoji uses the fallback without repeatedly reloading the atlas");
        bool indexed = font.glyphCount >= 95 + 96;
        for (int c = 32; c <= 126 && indexed; c++)
            indexed = font.glyphs[GetGlyphIndex(font, c)].value == c;
        for (int c = 0xa0; c <= 0xff && indexed; c++)
            indexed = font.glyphs[GetGlyphIndex(font, c)].value == c;
        Check(tool, indexed,
              "every glyph in the atlas answers to its own codepoint, ASCII and Latin-1 alike");
        int symbols[] = {0x2192, 0x3a9, 0x4e2d};
        for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
        {
            GlyphInfo glyph = font.glyphs[GetGlyphIndex(font, symbols[i])];
            Check(tool, glyph.value == symbols[i] && glyph.image.data && glyph.offsetY == 0,
                  "pasted arrows, Greek and CJK have their own correctly aligned bitmap glyphs");
        }
        input->text[0] = 'z';
        input->textCount = 1;
        input->pressed[KEY_HOME] = true;
    }
    else if (frame == 57)
        Check(tool, tool->document.elements[2].label[0] == 'z',
              "Home moves the caret, so typing lands where the caret is, not at the end");
    else if (frame == 58)
    {
        input->down[KEY_LEFT_CONTROL] = true;
        input->pressed[KEY_A] = true;
    }
    else if (frame == 59)
    {
        char limit[68];
        memset(limit, 'x', 62);
        memcpy(limit + 62, "\xe2\x86\x92", 4);
        SetClipboardText(limit);
        input->down[KEY_LEFT_CONTROL] = true;
        input->pressed[KEY_V] = true;
    }
    else if (frame == 60)
        Check(tool, strlen(tool->document.elements[2].label) == 62 &&
                        tool->document.elements[2].label[61] == 'x',
              "a full field stops before a multibyte character instead of pasting half of it");
    else if (frame == 41)
    {
        UiElement *element = &tool->document.elements[2];
        UiRect inner = ContainerContent(tool, &tool->document.elements[3]);
        Check(tool, element->rect.x >= inner.x && element->rect.y >= inner.y &&
                        element->rect.x + element->rect.width <= inner.x + inner.width,
              "a real drag lands the element inside the indent it was dropped on");
        input->mouseReleased[MOUSE_BUTTON_LEFT] = true;
    }
}

static bool Init(void *context)
{
    Tool *tool = context;
    tool->selected = -1;
    tool->creating = -1;
    tool->zoom = 1;
    tool->snap = true;
    tool->grid = true;
    tool->palette = (UiPanel){{MARGIN, MARGIN, PALETTE_WIDTH, 0}, false};
    tool->inspector = (UiPanel){{0, MARGIN, INSPECTOR_WIDTH, 0}, false};
    if (!UiInit(&tool->ui, UiThemeDefault()))
        return false;
    tool->gridStep = tool->ui.theme.containerGap;
    // Smoke runs must not depend on whatever layout happens to be on disk.
    if (tool->smoke || !UiDocumentLoad(&tool->document, LAYOUT_PATH))
    {
        UiDocumentInit(&tool->document, 320, 200, UI_SURFACE_WINDOW, "Layout");
        int gap = tool->ui.theme.containerGap;
        AddAt(tool, UI_ELEMENT_LABEL, gap, gap);
        AddAt(tool, UI_ELEMENT_BUTTON, gap, gap + tool->ui.theme.itemHeight + gap);
        ClearUndo(tool);
        tool->selected = 0;
        Say(tool, "New layout");
    }
    else
        Say(tool, "Loaded %s", LAYOUT_PATH);
    if (tool->smoke)
        SmokeChecks(tool);
    return true;
}

static void FrameInput(void *context, const EngineInput *input)
{
    Tool *tool = context;
    if (!tool->smoke)
        tool->input = *input;
}

static bool Update(void *context, double dt, const EngineInput *input)
{
    (void)dt;
    Tool *tool = context;
    if (tool->smoke)
        return tool->frames < 62 && !tool->failures;
    // Text entry owns the keyboard while a field has focus.
    if (!UiTextFieldActive(&tool->ui))
        Shortcuts(tool, input);
    return !tool->quit;
}

static void Draw(void *context, float alpha)
{
    (void)alpha;
    Tool *tool = context;
    const UiTheme *theme = &tool->ui.theme;
    UiRect screen = {0, 0, GetScreenWidth(), GetScreenHeight()};
    ClearBackground((Color){32, 34, 38, 255});
    if (tool->smoke)
        SmokeFrame(tool);
    UiBeginFrame(&tool->ui, &tool->input, screen);

    UiRect toolbarBounds = {MARGIN, MARGIN, MaxInt(0, screen.width - MARGIN * 2), 0};
    int toolbarHeight = Toolbar(tool, toolbarBounds, false);
    int panelTop = MARGIN + toolbarHeight + theme->containerGap;
    int panelBottom = screen.height - MARGIN - STATUS_HEIGHT - theme->containerGap;
    tool->palette.rect =
        (UiRect){MARGIN, panelTop, PALETTE_WIDTH,
                 MinInt(MaxInt(0, panelBottom - panelTop),
                        theme->frameWidth * 2 + theme->titleHeight +
                            (UI_ELEMENT_COUNT + 2) * theme->itemHeight +
                            theme->containerGap * 4 + theme->indentWidth * 2)};
    tool->inspector.rect = (UiRect){screen.width - INSPECTOR_WIDTH - MARGIN, panelTop,
                                    INSPECTOR_WIDTH, MaxInt(0, panelBottom - panelTop)};

    int workspaceX = tool->palette.rect.x + PALETTE_WIDTH + MARGIN;
    tool->workspace = (UiRect){workspaceX, panelTop,
                               MaxInt(0, tool->inspector.rect.x - MARGIN - workspaceX),
                               MaxInt(0, panelBottom - panelTop)};

    UiRect outer = UiDocumentOuterRectSized(&tool->ui, &tool->document, (Vector2){0, 0},
                                            TestWidth(tool), TestHeight(tool));
    tool->content = UiDocumentContentRectSized(&tool->ui, &tool->document, (Vector2){0, 0},
                                               TestWidth(tool), TestHeight(tool));
    int captionRoom = theme->itemHeight;
    int zoomedWidth = outer.width * tool->zoom;
    int zoomedHeight = outer.height * tool->zoom;

    if (tool->input.mousePressed[MOUSE_BUTTON_MIDDLE] &&
        PointIn(tool->workspace, tool->input.mousePosition))
        tool->panning = true;
    if (!tool->input.mouseDown[MOUSE_BUTTON_MIDDLE])
        tool->panning = false;
    if (tool->panning)
    {
        tool->pan.x += tool->input.mouseDelta.x;
        tool->pan.y += tool->input.mouseDelta.y;
    }

    Vector2 centred = {(float)(tool->workspace.x + (tool->workspace.width - zoomedWidth) / 2),
                       (float)(tool->workspace.y + captionRoom +
                               (tool->workspace.height - captionRoom - zoomedHeight) / 2)};
    // Panning cannot lose the surface: a corner of it always stays in the workspace.
    float keep = (float)MinInt(theme->itemHeight * 2, MinInt(zoomedWidth, zoomedHeight));
    tool->origin.x = centred.x + tool->pan.x;
    tool->origin.y = centred.y + tool->pan.y;
    tool->origin.x = (float)ClampInt((int)tool->origin.x, tool->workspace.x - zoomedWidth + (int)keep,
                                     tool->workspace.x + tool->workspace.width - (int)keep);
    tool->origin.y = (float)ClampInt((int)tool->origin.y,
                                     tool->workspace.y + captionRoom - zoomedHeight + (int)keep,
                                     tool->workspace.y + tool->workspace.height - (int)keep);
    tool->pan.x = tool->origin.x - centred.x;
    tool->pan.y = tool->origin.y - centred.y;
    // A window being resized from its corner keeps its top-left where it is.
    if (tool->resizing)
    {
        tool->origin = tool->resizeOrigin;
        tool->pan.x = tool->origin.x - centred.x;
        tool->pan.y = tool->origin.y - centred.y;
    }
    tool->pointer = (Vector2){(tool->input.mousePosition.x - tool->origin.x) / (float)tool->zoom,
                              (tool->input.mousePosition.y - tool->origin.y) / (float)tool->zoom};
    tool->pointerInWorkspace = PointIn(tool->workspace, tool->input.mousePosition);

    // Take the drag before the geometry is committed, so the frame grows on the same frame.
    ResizeSurface(tool, outer);
    outer = UiDocumentOuterRectSized(&tool->ui, &tool->document, (Vector2){0, 0}, TestWidth(tool),
                                     TestHeight(tool));
    tool->content = UiDocumentContentRectSized(&tool->ui, &tool->document, (Vector2){0, 0},
                                               TestWidth(tool), TestHeight(tool));

    if (tool->workspace.width > 0 && tool->workspace.height > 0 &&
        EnsureCanvas(tool, outer.width, outer.height))
    {
        // Edits happen in surface pixels, so the UI sees the pointer in that space for this pass.
        EngineInput local = tool->input;
        local.mousePosition = tool->pointer;
        UiBeginFrame(&tool->ui, &local, (UiRect){0, 0, outer.width, outer.height});
        BeginTextureMode(tool->canvas);
        DrawCanvas(tool);
        EndTextureMode();
        UiBeginFrame(&tool->ui, &tool->input, screen);
        DrawWorkspace(tool);
    }

    // Chrome is drawn last so an oversized surface is clipped by the panels instead of covering them.
    Toolbar(tool, toolbarBounds, true);
    UiDrawPanel(&tool->ui, &tool->palette, "Elements", DrawPaletteContents, tool);
    UiDrawPanel(&tool->ui, &tool->inspector, "Properties", DrawInspectorContents, tool);
    DrawStatus(tool, (UiRect){MARGIN, screen.height - MARGIN - STATUS_HEIGHT,
                              MaxInt(0, screen.width - MARGIN * 2), STATUS_HEIGHT});

    if (tool->input.mousePressed[MOUSE_BUTTON_RIGHT])
        tool->contextPoint = (Vector2){tool->pointer.x - tool->content.x,
                                       tool->pointer.y - tool->content.y};
    if (!tool->preview)
        UiContextMenu(&tool->ui, tool->workspace, "Surface", &canvasMenu, tool);
    UiDrawMenus(&tool->ui);
    UiEndFrame(&tool->ui);

    if (tool->smoke && tool->frames == 55)
    {
        EndScissorMode();
        Image shot = LoadImageFromScreen();
        ExportImage(shot, "build/core/ui_tool_text.png");
        UnloadImage(shot);
    }
    if (tool->smoke &&
        (tool->frames == 14 || tool->frames == 19 || tool->frames == 23 || tool->frames == 25 ||
         tool->frames == 47))
    {
        EndScissorMode(); // flush the pending batch so the capture holds this frame
        Image shot = LoadImageFromScreen();
        ExportImage(shot, tool->frames == 14   ? "build/core/ui_tool_drag.png"
                          : tool->frames == 19 ? "build/core/ui_tool.png"
                          : tool->frames == 23 ? "build/core/ui_tool_preview.png"
                          : tool->frames == 25 ? "build/core/ui_tool_screen.png"
                                               : "build/core/ui_tool_resize.png");
        UnloadImage(shot);
    }
    tool->frames++;
}

static void Shutdown(void *context)
{
    Tool *tool = context;
    ClearUndo(tool);
    if (tool->canvas.id)
        UnloadRenderTexture(tool->canvas);
    free(tool->scratch);
    UiDocumentFree(&tool->document);
    UiFree(&tool->ui);
    if (tool->smoke)
        TraceLog(LOG_INFO, "UI TOOL failures=%d frames=%d", tool->failures, tool->frames);
}

int main(int argc, char **argv)
{
    Tool tool = {0};
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--smoke"))
            tool.smoke = true;
        else
        {
            fprintf(stderr, "Usage: %s [--smoke]\n", argv[0]);
            return 1;
        }
    }
    EngineConfig config = {.engine_path = "engine",
                           .width = 1100,
                           .height = 700,
                           .targetFps = 60,
                           .fixed_dt = 0,
                           .max_frame_dt = 0.25,
                           .windowFlags = FLAG_WINDOW_RESIZABLE};
    EngineProject project = {Init, FrameInput, Update, Draw, Shutdown};
    int result = EngineRun(&config, &project, &tool);
    return result || tool.failures ? 1 : 0;
}
