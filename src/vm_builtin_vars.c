#include "vm_builtin_vars.h"

#include <time.h>

#include "runner.h"
#include "instance.h"
#include "collision.h"

#define MAX_VIEWS 8
#define MAX_BACKGROUNDS 8

#define REQUIRE_INSTANCE(inst) \
    if ((inst) == nullptr) return RValue_makeReal(0.0)

#define REQUIRE_RUNNER(runner) \
    if ((runner) == nullptr) return RValue_makeReal(0.0)

#define CUR_INSTANCE_VAR(typ, nam) \
    REQUIRE_INSTANCE(ctx->currentInstance); \
    return RValue_make##typ(ctx->currentInstance->nam)

#define CUR_INSTANCE_VAR_CAST(typ, cast, nam) \
    REQUIRE_INSTANCE(ctx->currentInstance); \
    return RValue_make##typ((cast) ctx->currentInstance->nam)

#define CUR_RUNNER_VAR(typ, nam) \
    REQUIRE_RUNNER(ctx->runner); \
    return RValue_make##typ(ctx->runner->nam)

#define CUR_RUNNER_VAR_CAST(typ, cast, nam) \
    REQUIRE_RUNNER(ctx->runner); \
    return RValue_make##typ((cast) ctx->runner->nam)

#define RUNNER_VIEW_VAR(nam, oob_retval) \
    REQUIRE_RUNNER(ctx->runner); \
    if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) { \
        return RValue_makeReal((GMLReal) ctx->runner->currentRoom->views[arrayIndex].nam); \
    } \
    return RValue_makeReal(oob_retval)

#define RUNNER_BG_VAR(nam, oob_retval) \
    REQUIRE_RUNNER(ctx->runner); \
    if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) { \
        return RValue_makeReal((GMLReal) ctx->runner->backgrounds[arrayIndex].nam); \
    } \
    return RValue_makeReal(oob_retval)

static bool isValidAlarmIndex(int alarmIndex) {
    return alarmIndex >= 0 && GML_ALARM_COUNT > alarmIndex;
}

_DECLARE_BUILTIN_GET_VAR(working_directory) {
    Runner* runner = (Runner*) ctx->runner;
    FileSystem* fs = runner->fileSystem;
    char* path = fs->vtable->resolvePath(fs, "");
    return RValue_makeOwnedString(path);
}

_DECLARE_BUILTIN_GET_VAR(os_type) {
    return RValue_makeReal(4.0); // os_linux
}

_DECLARE_BUILTIN_GET_VAR(os_windows) {
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(os_ps4) {
    return RValue_makeReal(6.0);
}

_DECLARE_BUILTIN_GET_VAR(os_psvita) {
    return RValue_makeReal(12.0);
}

_DECLARE_BUILTIN_GET_VAR(os_3ds) {
    return RValue_makeReal(14.0);
}

_DECLARE_BUILTIN_GET_VAR(os_switch_) {
    return RValue_makeReal(19.0);
}

_DECLARE_BUILTIN_GET_VAR(image_speed) {
    CUR_INSTANCE_VAR(Real, imageSpeed);
}

_DECLARE_BUILTIN_GET_VAR(image_index) {
    CUR_INSTANCE_VAR(Real, imageIndex);
}

_DECLARE_BUILTIN_GET_VAR(image_xscale) {
    CUR_INSTANCE_VAR(Real, imageXscale);
}

_DECLARE_BUILTIN_GET_VAR(image_yscale) {
    CUR_INSTANCE_VAR(Real, imageYscale);
}

_DECLARE_BUILTIN_GET_VAR(image_angle) {
    CUR_INSTANCE_VAR(Real, imageAngle);
}

_DECLARE_BUILTIN_GET_VAR(image_alpha) {
    CUR_INSTANCE_VAR(Real, imageAlpha);
}

_DECLARE_BUILTIN_GET_VAR(image_blend) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, imageBlend);
}

_DECLARE_BUILTIN_GET_VAR(image_number) {
    Instance* inst = (Instance*) ctx->currentInstance;
    REQUIRE_INSTANCE(ctx->currentInstance);

    if (inst->spriteIndex >= 0) {
        Sprite* sprite = &ctx->runner->dataWin->sprt.sprites[inst->spriteIndex];
        return RValue_makeReal((GMLReal) sprite->textureCount);
    }
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(sprite_index) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, spriteIndex);
}

_DECLARE_BUILTIN_GET_VAR(sprite_width) {
    Instance* inst = (Instance*) ctx->currentInstance;
    REQUIRE_INSTANCE(ctx->currentInstance);

    Runner* runner = (Runner*) ctx->runner;
    if (inst->spriteIndex >= 0 && runner != nullptr && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
        return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].width * inst->imageXscale);
    }
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(sprite_height) {
    Instance* inst = (Instance*) ctx->currentInstance;
    REQUIRE_INSTANCE(ctx->currentInstance);

    Runner* runner = (Runner*) ctx->runner;
    if (inst->spriteIndex >= 0 && runner != nullptr && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
        return RValue_makeReal((GMLReal) runner->dataWin->sprt.sprites[inst->spriteIndex].height * inst->imageYscale);
    }
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(bbox_left) {
    Instance* inst = (Instance*) ctx->currentInstance;
    REQUIRE_INSTANCE(ctx->currentInstance);

    Runner* runner = (Runner*) ctx->runner;
    if (runner != nullptr) {
        InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
        if (bbox.valid) {
            return RValue_makeReal(bbox.left);
        }
    }
    return RValue_makeReal(inst->x);
}

_DECLARE_BUILTIN_GET_VAR(bbox_right) {
    Instance* inst = (Instance*) ctx->currentInstance;
    REQUIRE_INSTANCE(ctx->currentInstance);

    Runner* runner = (Runner*) ctx->runner;
    if (runner != nullptr) {
        InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
        if (bbox.valid) {
            return RValue_makeReal(bbox.right);
        }
    }
    return RValue_makeReal(inst->x);
}

_DECLARE_BUILTIN_GET_VAR(bbox_top) {
    Instance* inst = (Instance*) ctx->currentInstance;
    REQUIRE_INSTANCE(ctx->currentInstance);

    Runner* runner = (Runner*) ctx->runner;
    if (runner != nullptr) {
        InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
        if (bbox.valid) {
            return RValue_makeReal(bbox.top);
        }
    }
    return RValue_makeReal(inst->y);
}

_DECLARE_BUILTIN_GET_VAR(bbox_bottom) {
    Instance* inst = (Instance*) ctx->currentInstance;
    REQUIRE_INSTANCE(ctx->currentInstance);

    Runner* runner = (Runner*) ctx->runner;
    if (runner != nullptr) {
        InstanceBBox bbox = Collision_computeBBox(runner->dataWin, inst);
        if (bbox.valid) {
            return RValue_makeReal(bbox.bottom);
        }
    }
    return RValue_makeReal(inst->y);
}

_DECLARE_BUILTIN_GET_VAR(visible) {
    CUR_INSTANCE_VAR(Bool, visible);
}

_DECLARE_BUILTIN_GET_VAR(depth) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, depth);
}

_DECLARE_BUILTIN_GET_VAR(x) {
    CUR_INSTANCE_VAR(Real, x);
}

_DECLARE_BUILTIN_GET_VAR(y) {
    CUR_INSTANCE_VAR(Real, y);
}

_DECLARE_BUILTIN_GET_VAR(xprevious) {
    CUR_INSTANCE_VAR(Real, xprevious);
}

_DECLARE_BUILTIN_GET_VAR(yprevious) {
    CUR_INSTANCE_VAR(Real, yprevious);
}

_DECLARE_BUILTIN_GET_VAR(xstart) {
    CUR_INSTANCE_VAR(Real, xstart);
}

_DECLARE_BUILTIN_GET_VAR(ystart) {
    CUR_INSTANCE_VAR(Real, ystart);
}

_DECLARE_BUILTIN_GET_VAR(mask_index) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, maskIndex);
}

_DECLARE_BUILTIN_GET_VAR(id) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, instanceId);
}

_DECLARE_BUILTIN_GET_VAR(object_index) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, objectIndex);
}

_DECLARE_BUILTIN_GET_VAR(persistent) {
    CUR_INSTANCE_VAR(Bool, persistent);
}

_DECLARE_BUILTIN_GET_VAR(solid) {
    CUR_INSTANCE_VAR(Bool, solid);
}

_DECLARE_BUILTIN_GET_VAR(speed) {
    CUR_INSTANCE_VAR(Real, speed);
}

_DECLARE_BUILTIN_GET_VAR(direction) {
    CUR_INSTANCE_VAR(Real, direction);
}

_DECLARE_BUILTIN_GET_VAR(hspeed) {
    CUR_INSTANCE_VAR(Real, hspeed);
}

_DECLARE_BUILTIN_GET_VAR(vspeed) {
    CUR_INSTANCE_VAR(Real, vspeed);
}

_DECLARE_BUILTIN_GET_VAR(friction) {
    CUR_INSTANCE_VAR(Real, friction);
}

_DECLARE_BUILTIN_GET_VAR(gravity) {
    CUR_INSTANCE_VAR(Real, gravity);
}

_DECLARE_BUILTIN_GET_VAR(gravity_direction) {
    CUR_INSTANCE_VAR(Real, gravityDirection);
}

_DECLARE_BUILTIN_GET_VAR(alarm) {
    REQUIRE_INSTANCE(ctx->currentInstance);

    if (isValidAlarmIndex(arrayIndex)) {
        return RValue_makeReal((GMLReal) ctx->currentInstance->alarm[arrayIndex]);
    }
    return RValue_makeReal(-1.0);
}

_DECLARE_BUILTIN_GET_VAR(path_index) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, pathIndex);
}

_DECLARE_BUILTIN_GET_VAR(path_position) {
    CUR_INSTANCE_VAR(Real, pathPosition);
}

_DECLARE_BUILTIN_GET_VAR(path_positionprevious) {
    CUR_INSTANCE_VAR(Real, pathPositionPrevious);
}

_DECLARE_BUILTIN_GET_VAR(path_speed) {
    CUR_INSTANCE_VAR(Real, pathSpeed);
}

_DECLARE_BUILTIN_GET_VAR(path_scale) {
    CUR_INSTANCE_VAR(Real, pathScale);
}

_DECLARE_BUILTIN_GET_VAR(path_orientation) {
    CUR_INSTANCE_VAR(Real, pathOrientation);
}

_DECLARE_BUILTIN_GET_VAR(path_endaction) {
    CUR_INSTANCE_VAR_CAST(Real, GMLReal, pathEndAction);
}

_DECLARE_BUILTIN_GET_VAR(room) {
    CUR_RUNNER_VAR_CAST(Real, GMLReal, currentRoomIndex);
}

_DECLARE_BUILTIN_GET_VAR(room_speed) {
    CUR_RUNNER_VAR_CAST(Real, GMLReal, currentRoom->speed);
}

_DECLARE_BUILTIN_GET_VAR(room_width) {
    CUR_RUNNER_VAR_CAST(Real, GMLReal, currentRoom->width);
}

_DECLARE_BUILTIN_GET_VAR(room_height) {
    CUR_RUNNER_VAR_CAST(Real, GMLReal, currentRoom->height);
}

_DECLARE_BUILTIN_GET_VAR(room_persistent) {
    CUR_RUNNER_VAR(Bool, currentRoom->persistent);
}

_DECLARE_BUILTIN_GET_VAR(view_current) {
    CUR_RUNNER_VAR_CAST(Real, GMLReal, viewCurrent);
}

_DECLARE_BUILTIN_GET_VAR(view_xview) {
    RUNNER_VIEW_VAR(viewX, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_yview) {
    RUNNER_VIEW_VAR(viewY, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_wview) {
    RUNNER_VIEW_VAR(viewWidth, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_hview) {
    RUNNER_VIEW_VAR(viewHeight, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_xport) {
    RUNNER_VIEW_VAR(portX, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_yport) {
    RUNNER_VIEW_VAR(portY, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_wport) {
    RUNNER_VIEW_VAR(portWidth, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_hport) {
    RUNNER_VIEW_VAR(portHeight, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_visible) {
    REQUIRE_RUNNER(ctx->runner);
    if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
        return RValue_makeBool(ctx->runner->currentRoom->views[arrayIndex].enabled);
    }
    return RValue_makeBool(false);
}

_DECLARE_BUILTIN_GET_VAR(view_angle) {
    REQUIRE_RUNNER(ctx->runner);
    if (arrayIndex >= 0 && MAX_VIEWS > arrayIndex) {
        return RValue_makeReal((GMLReal) ctx->runner->viewAngles[arrayIndex]);
    }
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_hborder) {
    RUNNER_VIEW_VAR(borderX, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_vborder) {
    RUNNER_VIEW_VAR(borderY, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_object) {
    RUNNER_VIEW_VAR(objectId, -4.0);
}

_DECLARE_BUILTIN_GET_VAR(view_hspeed) {
    RUNNER_VIEW_VAR(speedX, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(view_vspeed) {
    RUNNER_VIEW_VAR(speedY, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(background_visible) {
    REQUIRE_RUNNER(ctx->runner);
    if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) {
        return RValue_makeBool(ctx->runner->backgrounds[arrayIndex].visible);
    }
    return RValue_makeBool(false);
}

_DECLARE_BUILTIN_GET_VAR(background_index) {
    RUNNER_BG_VAR(backgroundIndex, -1.0);
}

_DECLARE_BUILTIN_GET_VAR(background_x) {
    RUNNER_BG_VAR(x, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(background_y) {
    RUNNER_BG_VAR(y, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(background_hspeed) {
    RUNNER_BG_VAR(speedX, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(background_vspeed) {
    RUNNER_BG_VAR(speedY, 0.0);
}

_DECLARE_BUILTIN_GET_VAR(background_width) {
    REQUIRE_RUNNER(ctx->runner);
    if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) {
        int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(ctx->runner->dataWin, ctx->runner->backgrounds[arrayIndex].backgroundIndex);
        if (tpagIndex >= 0) return RValue_makeReal((GMLReal) ctx->runner->dataWin->tpag.items[tpagIndex].boundingWidth);
    }
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(background_height) {
    REQUIRE_RUNNER(ctx->runner);
    if (arrayIndex >= 0 && MAX_BACKGROUNDS > arrayIndex) {
        int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(ctx->runner->dataWin, ctx->runner->backgrounds[arrayIndex].backgroundIndex);
        if (tpagIndex >= 0) return RValue_makeReal((GMLReal) ctx->runner->dataWin->tpag.items[tpagIndex].boundingHeight);
    }
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(background_alpha) {
    RUNNER_BG_VAR(alpha, 1.0);
}

_DECLARE_BUILTIN_GET_VAR(background_color) {
    CUR_RUNNER_VAR_CAST(Real, GMLReal, backgroundColor);
}

_DECLARE_BUILTIN_GET_VAR(current_time) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    GMLReal ms = (GMLReal) ts.tv_sec * 1000.0 + (GMLReal) ts.tv_nsec / 1000000.0;
    return RValue_makeReal(ms);
}

_DECLARE_BUILTIN_GET_VAR(argument_count) {
    return RValue_makeReal((GMLReal) ctx->scriptArgCount);
}

_DECLARE_BUILTIN_GET_VAR(argument) {
    if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > arrayIndex && arrayIndex >= 0) {
        RValue val = ctx->scriptArgs[arrayIndex];
        val.ownsString = false;
        return val;
    }
    return RValue_makeUndefined();
}

_DECLARE_BUILTIN_GET_VAR(argument0) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 0);
}

_DECLARE_BUILTIN_GET_VAR(argument1) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 1);
}

_DECLARE_BUILTIN_GET_VAR(argument2) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 2);
}

_DECLARE_BUILTIN_GET_VAR(argument3) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 3);
}

_DECLARE_BUILTIN_GET_VAR(argument4) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 4);
}

_DECLARE_BUILTIN_GET_VAR(argument5) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 5);
}

_DECLARE_BUILTIN_GET_VAR(argument6) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 6);
}

_DECLARE_BUILTIN_GET_VAR(argument7) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 7);
}

_DECLARE_BUILTIN_GET_VAR(argument8) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 8);
}

_DECLARE_BUILTIN_GET_VAR(argument9) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 9);
}

_DECLARE_BUILTIN_GET_VAR(argument10) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 10);
}

_DECLARE_BUILTIN_GET_VAR(argument11) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 11);
}

_DECLARE_BUILTIN_GET_VAR(argument12) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 12);
}

_DECLARE_BUILTIN_GET_VAR(argument13) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 13);
}

_DECLARE_BUILTIN_GET_VAR(argument14) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 14);
}

_DECLARE_BUILTIN_GET_VAR(argument15) {
    return BUILTIN_VAR(Get, argument)(ctx, name, 15);
}

_DECLARE_BUILTIN_GET_VAR(keyboard_key) {
    return RValue_makeReal((GMLReal) ctx->runner->keyboard->lastKey);
}

_DECLARE_BUILTIN_GET_VAR(keyboard_lastkey) {
    return RValue_makeReal((GMLReal) ctx->runner->keyboard->lastKey);
}

_DECLARE_BUILTIN_GET_VAR(application_surface) {
    return RValue_makeReal(-1.0); // sentinel ID for the application surface
}

_DECLARE_BUILTIN_GET_VAR(true) {
    return RValue_makeBool(true);
}

_DECLARE_BUILTIN_GET_VAR(false) {
    return RValue_makeBool(false);
}

_DECLARE_BUILTIN_GET_VAR(pi) {
    return RValue_makeReal(3.14159265358979323846);
}

_DECLARE_BUILTIN_GET_VAR(undefined) {
    return RValue_makeUndefined();
}

_DECLARE_BUILTIN_GET_VAR(path_action_stop) {
    return RValue_makeReal(0.0);
}

_DECLARE_BUILTIN_GET_VAR(path_action_restart) {
    return RValue_makeReal(1.0);
}

_DECLARE_BUILTIN_GET_VAR(path_action_continue) {
    return RValue_makeReal(2.0);
}

_DECLARE_BUILTIN_GET_VAR(path_action_reverse) {
    return RValue_makeReal(3.0);
}

_DECLARE_BUILTIN_GET_VAR(fps) {
    return RValue_makeReal(ctx->dataWin->gen8.gms2FPS);
}

_DECLARE_BUILTIN_SET_VAR(working_directory) {

}

_DECLARE_BUILTIN_SET_VAR(os_type) {

}

_DECLARE_BUILTIN_SET_VAR(os_windows) {

}

_DECLARE_BUILTIN_SET_VAR(os_ps4) {

}

_DECLARE_BUILTIN_SET_VAR(os_psvita) {

}

_DECLARE_BUILTIN_SET_VAR(os_3ds) {

}

_DECLARE_BUILTIN_SET_VAR(os_switch_) {

}

_DECLARE_BUILTIN_SET_VAR(image_speed) {

}

_DECLARE_BUILTIN_SET_VAR(image_index) {

}

_DECLARE_BUILTIN_SET_VAR(image_xscale) {

}

_DECLARE_BUILTIN_SET_VAR(image_yscale) {

}

_DECLARE_BUILTIN_SET_VAR(image_angle) {

}

_DECLARE_BUILTIN_SET_VAR(image_alpha) {

}

_DECLARE_BUILTIN_SET_VAR(image_blend) {

}

_DECLARE_BUILTIN_SET_VAR(image_number) {

}

_DECLARE_BUILTIN_SET_VAR(sprite_index) {

}

_DECLARE_BUILTIN_SET_VAR(sprite_width) {

}

_DECLARE_BUILTIN_SET_VAR(sprite_height) {

}

_DECLARE_BUILTIN_SET_VAR(bbox_left) {
    Instance* inst = (Instance*) ctx->currentInstance;
}

_DECLARE_BUILTIN_SET_VAR(bbox_right) {
    Instance* inst = (Instance*) ctx->currentInstance;
}

_DECLARE_BUILTIN_SET_VAR(bbox_top) {
    Instance* inst = (Instance*) ctx->currentInstance;
}

_DECLARE_BUILTIN_SET_VAR(bbox_bottom) {
    Instance* inst = (Instance*) ctx->currentInstance;
}

_DECLARE_BUILTIN_SET_VAR(BBox) {

}

_DECLARE_BUILTIN_SET_VAR(visible) {

}

_DECLARE_BUILTIN_SET_VAR(depth) {

}

_DECLARE_BUILTIN_SET_VAR(x) {

}

_DECLARE_BUILTIN_SET_VAR(y) {

}

_DECLARE_BUILTIN_SET_VAR(xprevious) {

}

_DECLARE_BUILTIN_SET_VAR(yprevious) {

}

_DECLARE_BUILTIN_SET_VAR(xstart) {

}

_DECLARE_BUILTIN_SET_VAR(ystart) {

}

_DECLARE_BUILTIN_SET_VAR(mask_index) {

}

_DECLARE_BUILTIN_SET_VAR(id) {

}

_DECLARE_BUILTIN_SET_VAR(object_index) {

}

_DECLARE_BUILTIN_SET_VAR(persistent) {

}

_DECLARE_BUILTIN_SET_VAR(solid) {

}

_DECLARE_BUILTIN_SET_VAR(speed) {

}

_DECLARE_BUILTIN_SET_VAR(direction) {

}

_DECLARE_BUILTIN_SET_VAR(hspeed) {

}

_DECLARE_BUILTIN_SET_VAR(vspeed) {

}

_DECLARE_BUILTIN_SET_VAR(friction) {

}

_DECLARE_BUILTIN_SET_VAR(gravity) {

}

_DECLARE_BUILTIN_SET_VAR(gravity_direction) {

}

_DECLARE_BUILTIN_SET_VAR(alarm) {

}

_DECLARE_BUILTIN_SET_VAR(path_index) {

}

_DECLARE_BUILTIN_SET_VAR(path_position) {

}

_DECLARE_BUILTIN_SET_VAR(path_positionprevious) {

}

_DECLARE_BUILTIN_SET_VAR(path_speed) {

}

_DECLARE_BUILTIN_SET_VAR(path_scale) {

}

_DECLARE_BUILTIN_SET_VAR(path_orientation) {

}

_DECLARE_BUILTIN_SET_VAR(path_endaction) {

}

_DECLARE_BUILTIN_SET_VAR(room) {

}

_DECLARE_BUILTIN_SET_VAR(room_speed) {

}

_DECLARE_BUILTIN_SET_VAR(room_width) {

}

_DECLARE_BUILTIN_SET_VAR(room_height) {

}

_DECLARE_BUILTIN_SET_VAR(room_persistent) {

}

_DECLARE_BUILTIN_SET_VAR(view_current) {

}

_DECLARE_BUILTIN_SET_VAR(view_xview) {

}

_DECLARE_BUILTIN_SET_VAR(view_yview) {

}

_DECLARE_BUILTIN_SET_VAR(view_wview) {

}

_DECLARE_BUILTIN_SET_VAR(view_hview) {

}

_DECLARE_BUILTIN_SET_VAR(view_xport) {

}

_DECLARE_BUILTIN_SET_VAR(view_yport) {

}

_DECLARE_BUILTIN_SET_VAR(view_wport) {

}

_DECLARE_BUILTIN_SET_VAR(view_hport) {

}

_DECLARE_BUILTIN_SET_VAR(view_visible) {

}

_DECLARE_BUILTIN_SET_VAR(view_angle) {

}

_DECLARE_BUILTIN_SET_VAR(view_hborder) {

}

_DECLARE_BUILTIN_SET_VAR(view_vborder) {

}

_DECLARE_BUILTIN_SET_VAR(view_object) {

}

_DECLARE_BUILTIN_SET_VAR(view_hspeed) {

}

_DECLARE_BUILTIN_SET_VAR(view_vspeed) {

}

_DECLARE_BUILTIN_SET_VAR(background_visible) {

}

_DECLARE_BUILTIN_SET_VAR(background_index) {

}

_DECLARE_BUILTIN_SET_VAR(background_x) {

}

_DECLARE_BUILTIN_SET_VAR(background_y) {

}

_DECLARE_BUILTIN_SET_VAR(background_hspeed) {

}

_DECLARE_BUILTIN_SET_VAR(background_vspeed) {

}

_DECLARE_BUILTIN_SET_VAR(background_width) {

}

_DECLARE_BUILTIN_SET_VAR(background_height) {

}

_DECLARE_BUILTIN_SET_VAR(background_alpha) {

}

_DECLARE_BUILTIN_SET_VAR(background_color) {

}

_DECLARE_BUILTIN_SET_VAR(current_time) {

}

_DECLARE_BUILTIN_SET_VAR(argument_count) {

}

_DECLARE_BUILTIN_SET_VAR(argument) {

}

_DECLARE_BUILTIN_SET_VAR(argument0) {

}

_DECLARE_BUILTIN_SET_VAR(argument1) {

}

_DECLARE_BUILTIN_SET_VAR(argument2) {

}

_DECLARE_BUILTIN_SET_VAR(argument3) {

}

_DECLARE_BUILTIN_SET_VAR(argument4) {

}

_DECLARE_BUILTIN_SET_VAR(argument5) {

}

_DECLARE_BUILTIN_SET_VAR(argument6) {

}

_DECLARE_BUILTIN_SET_VAR(argument7) {

}

_DECLARE_BUILTIN_SET_VAR(argument8) {

}

_DECLARE_BUILTIN_SET_VAR(argument9) {

}

_DECLARE_BUILTIN_SET_VAR(argument10) {

}

_DECLARE_BUILTIN_SET_VAR(argument11) {

}

_DECLARE_BUILTIN_SET_VAR(argument12) {

}

_DECLARE_BUILTIN_SET_VAR(argument13) {

}

_DECLARE_BUILTIN_SET_VAR(argument14) {

}

_DECLARE_BUILTIN_SET_VAR(argument15) {

}

_DECLARE_BUILTIN_SET_VAR(keyboard_key) {

}

_DECLARE_BUILTIN_SET_VAR(keyboard_lastkey) {

}

_DECLARE_BUILTIN_SET_VAR(application_surface) {

}

_DECLARE_BUILTIN_SET_VAR(true) {

}

_DECLARE_BUILTIN_SET_VAR(false) {

}

_DECLARE_BUILTIN_SET_VAR(pi) {

}

_DECLARE_BUILTIN_SET_VAR(undefined) {

}

_DECLARE_BUILTIN_SET_VAR(path_action_stop) {

}

_DECLARE_BUILTIN_SET_VAR(path_action_restart) {

}

_DECLARE_BUILTIN_SET_VAR(path_action_continue) {

}

_DECLARE_BUILTIN_SET_VAR(path_action_reverse) {

}

_DECLARE_BUILTIN_SET_VAR(fps) {

}

