#include <math.h>

#include <lvgl.h>

#include <dtcore/dterr.h>

#include <dtcore/dterr.h>
#include <dtcore/dteventlogger.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtpicosdk_helper.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtcpu.h>
#include <dtmc_base/dtruntime.h>

#include <dtmc_base_demos/demo_helpers.h>
#include <dtmc_base_demos/demo_lvgl_card.h>

#define TAG demo_name

// the demo's privates
typedef struct demo_t
{
    demo_config_t config;

    int32_t cycle_number;

    bool got_first_read;
} demo_t;

// forward declare the demo's internal functions
static void
_create_lvgl_screen(void);
static void
_update_readouts(lv_timer_t* timer);

// --------------------------------------------------------------------------------------
// return a string description of the demo (the returned string is heap allocated)
dterr_t*
demo_describe(demo_t* self, char** out_description)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_description);

    char* d = *out_description;
    char* s = "\n    ";
    d = dtstr_concat_format(d, s, "Description of the demo:");

    d = dtstr_concat_format(d, s, "This demo shows a simple card with two readouts showing fake fluctuating values.");
    d = dtstr_concat_format(d, s, "Two buttons are shown but they are not functional in this demo.");
    *out_description = d;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Create a new instance, allocating memory as needed
dterr_t*
demo_create(demo_t** self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    *self = (demo_t*)malloc(sizeof(demo_t));
    if (*self == NULL)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "failed to allocate %zu bytes for demo_t", sizeof(demo_t));
        goto cleanup;
    }

    memset(*self, 0, sizeof(demo_t));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Configure the demo instance with handles to implementations and settings
dterr_t*
demo_configure( //
  demo_t* self,
  demo_config_t* config)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(config);

    self->config = *config;
cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Run the demo logic (loops forever, blocking)
dterr_t*
demo_start(demo_t* self)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);

    // print the description of the demo
    {
        char* description = NULL;
        DTERR_C(dtmc_base_demo_helpers_decorate_description((void*)self, (demo_describe_fn)demo_describe, &description));
        dtlog_info(TAG, "%s", description);
    }

    _create_lvgl_screen();

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Stop, unregister and dispose of the demo instance resources
void
demo_dispose(demo_t* self)
{
    if (self == NULL)
        return;

    free(self);
}

// --------------------------------------------------------------------------------------

typedef struct app_ui_t
{
    lv_obj_t* temperature_value;
    lv_obj_t* brightness_value;
    float temperature;
    float brightness;
    int tick;
} app_ui_t;

// --------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------
static void
_update_readouts(lv_timer_t* timer)
{
    app_ui_t* ui = (app_ui_t*)lv_timer_get_user_data(timer);
    if (ui == NULL)
    {
        return;
    }

    ui->tick++;

    /* Fake values that gently fluctuate once per second */
    ui->temperature = 22.50f + 1.75f * sinf((float)ui->tick * 0.40f);
    ui->brightness = 55.00f + 8.25f * cosf((float)ui->tick * 0.33f);

    char temp_buf[32];
    char bright_buf[32];

    snprintf(temp_buf, sizeof(temp_buf), "%0.2f", ui->temperature);
    snprintf(bright_buf, sizeof(bright_buf), "%0.2f", ui->brightness);

    if (ui->temperature_value)
    {
        lv_label_set_text(ui->temperature_value, temp_buf);
    }

    if (ui->brightness_value)
    {
        lv_label_set_text(ui->brightness_value, bright_buf);
    }
}
// --------------------------------------------------------------------------------------
static void
_create_lvgl_screen(void)
{
    static app_ui_t ui = { 0 };

    lv_obj_t* screen = lv_screen_active();

    lv_obj_set_style_bg_color(screen, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* -------------------------------------------------------
       Root layout
    ------------------------------------------------------- */

    lv_obj_t* root = lv_obj_create(screen);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_center(root);

    lv_obj_set_layout(root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_pad_all(root, 10, 0);
    lv_obj_set_style_pad_row(root, 10, 0);

    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_shadow_width(root, 0, 0);

    /* -------------------------------------------------------
       Header
    ------------------------------------------------------- */

    lv_obj_t* header = lv_obj_create(root);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);

    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_shadow_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 4, 0);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Hello LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_center(title);

    /* -------------------------------------------------------
       Content area
    ------------------------------------------------------- */

    lv_obj_t* content = lv_obj_create(root);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);

    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 10, 0);

    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_shadow_width(content, 0, 0);

    /* -------------------------------------------------------
       Main card
    ------------------------------------------------------- */

    lv_obj_t* card = lv_obj_create(content);
    lv_obj_set_width(card, 220);
    lv_obj_set_height(card, LV_SIZE_CONTENT);

    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 8, 0);

    /* row 1 */
    lv_obj_t* row1 = lv_obj_create(card);
    lv_obj_remove_style_all(row1);
    lv_obj_set_width(row1, lv_pct(100));
    lv_obj_set_height(row1, LV_SIZE_CONTENT);
    lv_obj_set_layout(row1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* row1_label = lv_label_create(row1);
    lv_label_set_text(row1_label, "Temperature");

    lv_obj_t* row1_value = lv_label_create(row1);
    lv_label_set_text(row1_value, "23.68");
    ui.temperature_value = row1_value;

    /* row 2 */
    lv_obj_t* row2 = lv_obj_create(card);
    lv_obj_remove_style_all(row2);
    lv_obj_set_width(row2, lv_pct(100));
    lv_obj_set_height(row2, LV_SIZE_CONTENT);
    lv_obj_set_layout(row2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* row2_label = lv_label_create(row2);
    lv_label_set_text(row2_label, "Brightness");

    lv_obj_t* row2_value = lv_label_create(row2);
    lv_label_set_text(row2_value, "55.00");
    ui.brightness_value = row2_value;

    /* row 3 */
    lv_obj_t* row3 = lv_obj_create(card);
    lv_obj_remove_style_all(row3);
    lv_obj_set_width(row3, lv_pct(100));
    lv_obj_set_height(row3, LV_SIZE_CONTENT);
    lv_obj_set_layout(row3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* row3_label = lv_label_create(row3);
    lv_label_set_text(row3_label, "Status");

    lv_obj_t* row3_value = lv_label_create(row3);
    lv_label_set_text(row3_value, "Online");

    /* save references if you want to update later */
    (void)ui;

    /* -------------------------------------------------------
       Footer / action bar
    ------------------------------------------------------- */

    lv_obj_t* footer = lv_obj_create(root);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_height(footer, 50);

    lv_obj_set_layout(footer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_shadow_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);

    lv_obj_t* btn_on = lv_button_create(footer);
    lv_obj_set_size(btn_on, 90, 36);
    lv_obj_t* lbl_on = lv_label_create(btn_on);
    lv_label_set_text(lbl_on, "On");
    lv_obj_center(lbl_on);

    lv_obj_t* btn_off = lv_button_create(footer);
    lv_obj_set_size(btn_off, 90, 36);
    lv_obj_t* lbl_off = lv_label_create(btn_off);
    lv_label_set_text(lbl_off, "Off");
    lv_obj_center(lbl_off);

    lv_timer_create(_update_readouts, 1000, &ui);
}
