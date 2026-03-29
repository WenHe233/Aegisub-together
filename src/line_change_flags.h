#pragma once

struct LineChangeFlags {
    bool fn   = false;
    bool b    = false;
    bool i    = false;
    bool u    = false;
    bool s    = false;

    bool fs   = false;
    bool fscx = false;
    bool fscy = false;
    bool fsp  = false;

    bool Any() const {
        return fn || b || i || u || s || fs || fscx || fscy || fsp;
    }
};