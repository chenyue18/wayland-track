#include "../x11_tables.hpp"

#include <cstring>

const char* x11_req_name(int opcode) {
    for (int i = 0; i < WT_X11_REQ_NAME_COUNT; i++)
        if (WT_X11_REQ_NAMES[i].opcode == opcode) return WT_X11_REQ_NAMES[i].name;
    return nullptr;
}

static const char* lookup(const WTXNameEntry* tbl, int count, int opcode) {
    for (int i = 0; i < count; i++)
        if (tbl[i].opcode == opcode) return tbl[i].name;
    return nullptr;
}

static const WTXNameEntry kErrNames[] = {
    {1, "Request"}, {2, "Value"}, {3, "Window"}, {4, "Pixmap"}, {5, "Atom"},
    {6, "Cursor"}, {7, "Font"}, {8, "Match"}, {9, "Drawing"}, {10, "Access"},
    {11, "Alloc"}, {12, "Colormap"}, {13, "GContext"}, {14, "IDChoice"},
    {15, "Name"}, {16, "Length"}, {17, "Implementation"},
};

static const WTXNameEntry kEvtNames[] = {
    {2, "KeyPress"}, {3, "KeyRelease"}, {4, "ButtonPress"}, {5, "ButtonRelease"},
    {6, "MotionNotify"}, {7, "EnterNotify"}, {8, "LeaveNotify"}, {9, "FocusIn"},
    {10, "FocusOut"}, {11, "KeymapNotify"}, {12, "Expose"}, {13, "GraphicsExpose"},
    {14, "NoExpose"}, {15, "VisibilityNotify"}, {16, "CreateNotify"},
    {17, "DestroyNotify"}, {18, "UnmapNotify"}, {19, "MapNotify"},
    {20, "MapRequest"}, {21, "ReparentNotify"}, {22, "ConfigureNotify"},
    {23, "ConfigureRequest"}, {24, "GravityNotify"}, {25, "ResizeRequest"},
    {26, "CirculateNotify"}, {27, "CirculateRequest"}, {28, "PropertyNotify"},
    {29, "SelectionClear"}, {30, "SelectionRequest"}, {31, "SelectionNotify"},
    {32, "ColormapNotify"}, {33, "ClientMessage"}, {34, "MappingNotify"},
};

const char* x11_err_name(int opcode) { return lookup(kErrNames, sizeof(kErrNames) / sizeof(kErrNames[0]), opcode); }
const char* x11_evt_name(int opcode) { return lookup(kEvtNames, sizeof(kEvtNames) / sizeof(kEvtNames[0]), opcode); }
