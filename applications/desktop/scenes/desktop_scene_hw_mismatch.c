#include "../desktop_i.h"
#include <furi_hal_version.h>

#define HW_MISMATCH_BACK_EVENT (0UL)

void desktop_scene_hw_mismatch_callback(void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, HW_MISMATCH_BACK_EVENT);
}

void desktop_scene_hw_mismatch_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;
    furi_assert(desktop);
    furi_assert(!desktop->text_buffer);
    Popup* popup = desktop->hw_mismatch_popup;
    desktop->text_buffer = furi_alloc(256);
    snprintf(
        desktop->text_buffer,
        256,
        "HW target: %d\nFW target: %d",
        furi_hal_version_get_hw_target(),
        version_get_target(NULL));
    popup_set_context(popup, desktop);
    popup_set_header(
        popup, "!!!! HW Mismatch !!!!", 60, 14 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignCenter);
    popup_set_text(
        popup, desktop->text_buffer, 60, 37 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignCenter);
    popup_set_callback(popup, desktop_scene_hw_mismatch_callback);
    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewHwMismatch);
}

bool desktop_scene_hw_mismatch_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case HW_MISMATCH_BACK_EVENT:
            scene_manager_previous_scene(desktop->scene_manager);
            consumed = true;
            break;

        default:
            break;
        }
    }
    return consumed;
}

void desktop_scene_hw_mismatch_on_exit(void* context) {
    Desktop* desktop = (Desktop*)context;
    furi_assert(desktop);
    furi_assert(desktop->text_buffer);
    Popup* popup = desktop->hw_mismatch_popup;
    popup_set_header(popup, NULL, 0, 0, AlignCenter, AlignBottom);
    popup_set_text(popup, NULL, 0, 0, AlignCenter, AlignTop);
    popup_set_callback(popup, NULL);
    popup_set_context(popup, NULL);
    free(desktop->text_buffer);
    desktop->text_buffer = NULL;
}
