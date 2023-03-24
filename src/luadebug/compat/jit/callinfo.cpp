#include <lj_arch.h>
#include <lj_debug.h>
#include <lj_frame.h>
#include <lj_obj.h>

#include "compat/internal.h"

static cTValue* debug_frame(lua_State* L, int level, int* size) {
    cTValue *frame, *nextframe, *bot = tvref(L->stack) + LJ_FR2;
    /* Traverse frames backwards. */
    for (nextframe = frame = L->base - 1; frame > bot;) {
        if (frame_gc(frame) == obj2gco(L))
            level++; /* Skip dummy frames. See lj_err_optype_call(). */
        if (level-- == 0) {
            *size = (int)(nextframe - frame);
            return frame; /* Level found. */
        }
        nextframe = frame;
        if (frame_islua(frame)) {
            frame = frame_prevl(frame);
        }
        else {
            if (frame_isvarg(frame))
                level++; /* Skip vararg pseudo-frame. */
            frame = frame_prevd(frame);
        }
    }
    *size = level;
    return NULL; /* Level not found. */
}

CallInfo* lua_getcallinfo(lua_State* L) {
    int size;
    return const_cast<CallInfo*>(debug_frame(L, 0, &size));
}

Proto* lua_ci2proto(CallInfo* ci) {
    GCfunc* func = frame_func(ci);
    if (!isluafunc(func))
        return 0;
    return funcproto(func);
}

CallInfo* lua_debug2ci(lua_State* L, lua_Debug* ar) {
    uint32_t offset = (uint32_t)ar->i_ci & 0xffff;
    return tvref(L->stack) + offset;
}

int lua_isluafunc(lua_State* L, lua_Debug* ar) {
    return isluafunc(frame_func(lua_debug2ci(L, ar)));
}