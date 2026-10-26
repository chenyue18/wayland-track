#pragma once

struct WTXNameEntry {
    int opcode;
    const char* name;
};

extern const WTXNameEntry WT_X11_REQ_NAMES[];
extern const int WT_X11_REQ_NAME_COUNT;

const char* x11_req_name(int opcode);
const char* x11_err_name(int opcode);
const char* x11_evt_name(int opcode);
