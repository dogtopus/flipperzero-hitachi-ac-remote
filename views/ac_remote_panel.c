#include "ac_remote_panel.h"

#include <gui/canvas.h>
#include <gui/elements.h>

#include <furi.h>
#include <furi_hal_resources.h>
#include <stdint.h>

#include <m-array.h>
#include <m-i-list.h>
#include <m-list.h>

typedef struct {
    // uint16_t to support multi-screen, wide button panel
    int index;
    uint16_t x;
    uint16_t y;
    Font font;
    const char* str;
} LabelElement;

LIST_DEF(LabelList, LabelElement, M_POD_OPLIST)
#define M_OPL_LabelList_t() LIST_OPLIST(LabelList)

typedef struct {
    uint16_t x;
    uint16_t y;
    const Icon* name;
    const Icon* name_selected;
} IconElement;

LIST_DEF(IconList, IconElement, M_POD_OPLIST)
#define M_OPL_IconList_t() LIST_OPLIST(IconList)

typedef struct ButtonItem {
    uint16_t index;
    ButtonItemCallback callback;
    IconElement icon;
    void* callback_context;
} ButtonItem;

ARRAY_DEF(ButtonArray, ButtonItem*, M_PTR_OPLIST);
#define M_OPL_ButtonArray_t() ARRAY_OPLIST(ButtonArray, M_PTR_OPLIST)
ARRAY_DEF(ButtonMatrix, ButtonArray_t);
#define M_OPL_ButtonMatrix_t() ARRAY_OPLIST(ButtonMatrix, M_OPL_ButtonArray_t())

struct ACRemotePanel {
    View* view;
};

typedef struct {
    ButtonMatrix_t button_matrix;
    IconList_t icons;
    LabelList_t labels;
    uint16_t reserve_x;
    uint16_t reserve_y;
    uint16_t selected_item_x;
    uint16_t selected_item_y;
} ACRemotePanelModel;

static ButtonItem** ac_remote_panel_get_item(ACRemotePanelModel* model, size_t x, size_t y);
static void ac_remote_panel_process_up(ACRemotePanel* ac_remote_panel);
static void ac_remote_panel_process_down(ACRemotePanel* ac_remote_panel);
static void ac_remote_panel_process_left(ACRemotePanel* ac_remote_panel);
static void ac_remote_panel_process_right(ACRemotePanel* ac_remote_panel);
static void ac_remote_panel_process_ok(ACRemotePanel* ac_remote_panel, InputType event_type);
static void ac_remote_panel_view_draw_callback(Canvas* canvas, void* _model);
static bool ac_remote_panel_view_input_callback(InputEvent* event, void* context);

ACRemotePanel* ac_remote_panel_alloc() {
    ACRemotePanel* ac_remote_panel = malloc(sizeof(ACRemotePanel));
    ac_remote_panel->view = view_alloc();
    view_set_orientation(ac_remote_panel->view, ViewOrientationVertical);
    view_set_context(ac_remote_panel->view, ac_remote_panel);
    view_allocate_model(ac_remote_panel->view, ViewModelTypeLocking, sizeof(ACRemotePanelModel));
    view_set_draw_callback(ac_remote_panel->view, ac_remote_panel_view_draw_callback);
    view_set_input_callback(ac_remote_panel->view, ac_remote_panel_view_input_callback);

    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            model->reserve_x = 0;
            model->reserve_y = 0;
            model->selected_item_x = 0;
            model->selected_item_y = 0;
            ButtonMatrix_init(model->button_matrix);
            LabelList_init(model->labels);
        },
        true);

    return ac_remote_panel;
}

void ac_remote_panel_reset_selection(ACRemotePanel* ac_remote_panel) {
    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            model->selected_item_x = 0;
            model->selected_item_y = 0;
        },
        true);
}

void ac_remote_panel_reserve(ACRemotePanel* ac_remote_panel, size_t reserve_x, size_t reserve_y) {
    furi_check(reserve_x > 0);
    furi_check(reserve_y > 0);

    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            model->reserve_x = reserve_x;
            model->reserve_y = reserve_y;
            ButtonMatrix_reserve(model->button_matrix, model->reserve_y);
            for(size_t i = 0; i > model->reserve_y; ++i) {
                ButtonArray_t* array = ButtonMatrix_get(model->button_matrix, i);
                ButtonArray_init(*array);
                ButtonArray_reserve(*array, reserve_x);
            }
            LabelList_init(model->labels);
        },
        true);
}

void ac_remote_panel_free(ACRemotePanel* ac_remote_panel) {
    furi_assert(ac_remote_panel);

    ac_remote_panel_reset(ac_remote_panel);

    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            LabelList_clear(model->labels);
            ButtonMatrix_clear(model->button_matrix);
        },
        true);

    view_free(ac_remote_panel->view);
    free(ac_remote_panel);
}

void ac_remote_panel_reset(ACRemotePanel* ac_remote_panel) {
    furi_assert(ac_remote_panel);

    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            for(size_t x = 0; x < model->reserve_x; ++x) {
                for(size_t y = 0; y < model->reserve_y; ++y) {
                    ButtonItem** button_item = ac_remote_panel_get_item(model, x, y);
                    if(*button_item == NULL) {
                        continue;
                    }
                    free(*button_item);
                    *button_item = NULL;
                }
            }
            model->reserve_x = 0;
            model->reserve_y = 0;
            model->selected_item_x = 0;
            model->selected_item_y = 0;
            LabelList_reset(model->labels);
            IconList_reset(model->icons);
            ButtonMatrix_reset(model->button_matrix);
        },
        true);
}

static ButtonItem** ac_remote_panel_get_item(ACRemotePanelModel* model, size_t x, size_t y) {
    furi_assert(model);

    furi_check(x < model->reserve_x);
    furi_check(y < model->reserve_y);
    ButtonArray_t* button_array = ButtonMatrix_safe_get(model->button_matrix, x);
    ButtonItem** item = ButtonArray_safe_get(*button_array, y);
    return item;
}

void ac_remote_panel_add_item(
    ACRemotePanel* ac_remote_panel,
    uint16_t index,
    uint16_t matrix_place_x,
    uint16_t matrix_place_y,
    uint16_t x,
    uint16_t y,
    const Icon* icon_name,
    const Icon* icon_name_selected,
    ButtonItemCallback callback,
    void* callback_context) {
    furi_assert(ac_remote_panel);

    with_view_model( //-V773
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            ButtonItem** item_ptr =
                ac_remote_panel_get_item(model, matrix_place_x, matrix_place_y);
            furi_check(*item_ptr == NULL);
            *item_ptr = malloc(sizeof(ButtonItem));
            ButtonItem* item = *item_ptr;
            item->callback = callback;
            item->callback_context = callback_context;
            item->icon.x = x;
            item->icon.y = y;
            item->icon.name = icon_name;
            item->icon.name_selected = icon_name_selected;
            item->index = index;
        },
        true);
}

View* ac_remote_panel_get_view(ACRemotePanel* ac_remote_panel) {
    furi_assert(ac_remote_panel);
    return ac_remote_panel->view;
}

static void ac_remote_panel_view_draw_callback(Canvas* canvas, void* _model) {
    furi_assert(canvas);
    furi_assert(_model);

    ACRemotePanelModel* model = _model;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    for
        M_EACH(icon, model->icons, IconList_t) {
            canvas_draw_icon(canvas, icon->x, icon->y, icon->name);
        }

    for(size_t x = 0; x < model->reserve_x; ++x) {
        for(size_t y = 0; y < model->reserve_y; ++y) {
            ButtonItem* button_item = *ac_remote_panel_get_item(model, x, y);
            if(!button_item) {
                continue;
            }
            const Icon* icon_name = button_item->icon.name;
            if((model->selected_item_x == x) && (model->selected_item_y == y)) {
                icon_name = button_item->icon.name_selected;
            }
            canvas_draw_icon(canvas, button_item->icon.x, button_item->icon.y, icon_name);
        }
    }

    for
        M_EACH(label, model->labels, LabelList_t) {
            canvas_set_font(canvas, label->font);
            canvas_draw_str(canvas, label->x, label->y, label->str);
        }
}

static void ac_remote_panel_process_down(ACRemotePanel* ac_remote_panel) {
    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            uint16_t new_selected_item_x = model->selected_item_x;
            uint16_t new_selected_item_y = model->selected_item_y;
            size_t i;

            if(new_selected_item_y < (model->reserve_y - 1)) {
                ++new_selected_item_y;

                for(i = 0; i < model->reserve_x; ++i) {
                    new_selected_item_x = (model->selected_item_x + i) % model->reserve_x;
                    if(*ac_remote_panel_get_item(model, new_selected_item_x, new_selected_item_y)) {
                        break;
                    }
                }
                if(i != model->reserve_x) {
                    model->selected_item_x = new_selected_item_x;
                    model->selected_item_y = new_selected_item_y;
                }
            }
        },
        true);
}

static void ac_remote_panel_process_up(ACRemotePanel* ac_remote_panel) {
    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            size_t new_selected_item_x = model->selected_item_x;
            size_t new_selected_item_y = model->selected_item_y;
            size_t i;

            if(new_selected_item_y > 0) {
                --new_selected_item_y;

                for(i = 0; i < model->reserve_x; ++i) {
                    new_selected_item_x = (model->selected_item_x + i) % model->reserve_x;
                    if(*ac_remote_panel_get_item(model, new_selected_item_x, new_selected_item_y)) {
                        break;
                    }
                }
                if(i != model->reserve_x) {
                    model->selected_item_x = new_selected_item_x;
                    model->selected_item_y = new_selected_item_y;
                }
            }
        },
        true);
}

static void ac_remote_panel_process_left(ACRemotePanel* ac_remote_panel) {
    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            size_t new_selected_item_x = model->selected_item_x;
            size_t new_selected_item_y = model->selected_item_y;
            size_t i;

            if(new_selected_item_x > 0) {
                --new_selected_item_x;

                for(i = 0; i < model->reserve_y; ++i) {
                    new_selected_item_y = (model->selected_item_y + i) % model->reserve_y;
                    if(*ac_remote_panel_get_item(model, new_selected_item_x, new_selected_item_y)) {
                        break;
                    }
                }
                if(i != model->reserve_y) {
                    model->selected_item_x = new_selected_item_x;
                    model->selected_item_y = new_selected_item_y;
                }
            }
        },
        true);
}

static void ac_remote_panel_process_right(ACRemotePanel* ac_remote_panel) {
    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            uint16_t new_selected_item_x = model->selected_item_x;
            uint16_t new_selected_item_y = model->selected_item_y;
            size_t i;

            if(new_selected_item_x < (model->reserve_x - 1)) {
                ++new_selected_item_x;

                for(i = 0; i < model->reserve_y; ++i) {
                    new_selected_item_y = (model->selected_item_y + i) % model->reserve_y;
                    if(*ac_remote_panel_get_item(model, new_selected_item_x, new_selected_item_y)) {
                        break;
                    }
                }
                if(i != model->reserve_y) {
                    model->selected_item_x = new_selected_item_x;
                    model->selected_item_y = new_selected_item_y;
                }
            }
        },
        true);
}

void ac_remote_panel_process_ok(ACRemotePanel* ac_remote_panel, InputType event_type) {
    ButtonItem* button_item = NULL;

    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            button_item =
                *ac_remote_panel_get_item(model, model->selected_item_x, model->selected_item_y);
        },
        true);

    if(button_item && button_item->callback) {
        button_item->callback(button_item->callback_context, event_type, button_item->index);
    }
}

static bool ac_remote_panel_view_input_callback(InputEvent* event, void* context) {
    ACRemotePanel* ac_remote_panel = context;
    furi_assert(ac_remote_panel);
    bool consumed = false;

    if(event->type == InputTypeShort || event->type == InputTypeLong) {
        switch(event->key) {
        case InputKeyUp:
            consumed = true;
            ac_remote_panel_process_up(ac_remote_panel);
            break;
        case InputKeyDown:
            consumed = true;
            ac_remote_panel_process_down(ac_remote_panel);
            break;
        case InputKeyLeft:
            consumed = true;
            ac_remote_panel_process_left(ac_remote_panel);
            break;
        case InputKeyRight:
            consumed = true;
            ac_remote_panel_process_right(ac_remote_panel);
            break;
        case InputKeyOk:
            consumed = true;
            ac_remote_panel_process_ok(ac_remote_panel, event->type);
            break;
        default:
            break;
        }
    }

    return consumed;
}

void ac_remote_panel_add_label(
    ACRemotePanel* ac_remote_panel,
    int index,
    uint16_t x,
    uint16_t y,
    Font font,
    const char* label_str) {
    furi_assert(ac_remote_panel);

    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            LabelElement* label = LabelList_push_raw(model->labels);
            label->index = index;
            label->x = x;
            label->y = y;
            label->font = font;
            label->str = label_str;
        },
        true);
}

void ac_remote_panel_add_icon(
    ACRemotePanel* ac_remote_panel,
    uint16_t x,
    uint16_t y,
    const Icon* icon_name) {
    furi_assert(ac_remote_panel);

    with_view_model( //-V773
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            IconElement* icon = IconList_push_raw(model->icons);
            icon->x = x;
            icon->y = y;
            icon->name = icon_name;
            icon->name_selected = icon_name;
        },
        true);
}

void ac_remote_panel_item_set_icons(
    ACRemotePanel* ac_remote_panel,
    uint32_t index,
    const Icon* icon_name,
    const Icon* icon_name_selected) {
    furi_assert(ac_remote_panel);

    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            for(size_t x = 0; x < model->reserve_x; ++x) {
                for(size_t y = 0; y < model->reserve_y; ++y) {
                    ButtonItem** button_item = ac_remote_panel_get_item(model, x, y);
                    ButtonItem* item = *button_item;
                    if(item == NULL) {
                        continue;
                    }
                    if(item->index == index) {
                        item->icon.name = icon_name;
                        item->icon.name_selected = icon_name_selected;
                    }
                }
            }
        },
        true);
}

void ac_remote_panel_label_set_string(
    ACRemotePanel* ac_remote_panel,
    int index,
    const char* label_str) {
    with_view_model(
        ac_remote_panel->view,
        ACRemotePanelModel * model,
        {
            for
                M_EACH(label, model->labels, LabelList_t) {
                    if(label->index == index) {
                        label->str = label_str;
                    }
                }
        },
        true);
}

void ac_remote_panel_update_view(ACRemotePanel* ac_remote_panel) {
    view_get_model(ac_remote_panel->view);
    view_commit_model(ac_remote_panel->view, true);
}
