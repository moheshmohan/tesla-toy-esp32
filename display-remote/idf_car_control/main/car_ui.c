#include "car_ui.h"
#include "car_net.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <stdint.h>

// ============================= palette =====================================
#define C_BG1     0x0B1020   // background gradient top
#define C_BG2     0x05070D   // background gradient bottom
#define C_PANEL   0x141B2D
#define C_BORDER  0x273349
#define C_TEXT    0xFFFFFF
#define C_MUTED   0x8A93A6
#define C_CYAN    0x29E0FF   // headlight
#define C_CYAN_D  0x0B2E3A
#define C_AMBER   0xFFB020   // blinkers
#define C_AMBER_D 0x3A2A05
#define C_RED     0xFF3B30   // taillight
#define C_RED_D   0x3A0E0C
#define C_GREEN   0x30D158
#define C_SILVER  0xC7CEDB   // body
#define C_SILVER2 0x8B93A4   // body gradient bottom
#define C_GLASS   0x0E1524   // windows
#define C_ROOF    0x39424F   // roof panel
#define C_TIRE    0x0A0D14
#define C_SOCKET  0x3B4456   // unlit lamp

// ============================= state =======================================
static bool s_blinkerL = false, s_blinkerR = false;
static bool s_headlight = false, s_taillight = false;
static bool s_flash_on = true;
static int  s_hour = 21, s_min = 12;

// widgets we update at runtime
static lv_obj_t *lbl_time, *lbl_wifi, *lbl_status, *dot_status;
static lv_obj_t *car_headL, *car_headR, *car_tail;
static lv_obj_t *car_blink[4];   // 0=FL 1=FR 2=RL 3=RR
static lv_obj_t *btn[4];         // 0=BlinkL 1=BlinkR 2=Head 3=Tail

// shared styles
static lv_style_t st_btn, st_btn_amber, st_btn_cyan, st_btn_red;

// =========================== helpers =======================================
static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, int r,
                          uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// Turn a lamp object on (bright + colored glow) or off (dim socket, no glow).
static void glow(lv_obj_t *o, bool on, uint32_t color) {
    if (on) {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_color(o, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_width(o, 18, 0);
        lv_obj_set_style_shadow_spread(o, 2, 0);
        lv_obj_set_style_shadow_opa(o, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_shadow_width(o, 0, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(C_SOCKET), 0);
    }
}

static void update_car(void) {
    glow(car_headL, s_headlight, C_CYAN);
    glow(car_headR, s_headlight, C_CYAN);
    glow(car_tail,  s_taillight, C_RED);
    bool fl = s_blinkerL && s_flash_on;
    bool fr = s_blinkerR && s_flash_on;
    glow(car_blink[0], fl, C_AMBER);
    glow(car_blink[2], fl, C_AMBER);
    glow(car_blink[1], fr, C_AMBER);
    glow(car_blink[3], fr, C_AMBER);
}

static void reflect_btn(int id) {
    bool on = (id == 0) ? s_blinkerL : (id == 1) ? s_blinkerR
            : (id == 2) ? s_headlight : s_taillight;
    if (on) lv_obj_add_state(btn[id], LV_STATE_CHECKED);
    else    lv_obj_clear_state(btn[id], LV_STATE_CHECKED);
}

// =========================== callbacks =====================================
static void btn_event(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch (id) {
        case 0: s_blinkerL = !s_blinkerL;
                car_net_send(s_blinkerL ? "/blinker1/on" : "/blinker1/off"); break;
        case 1: s_blinkerR = !s_blinkerR;
                car_net_send(s_blinkerR ? "/blinker2/on" : "/blinker2/off"); break;
        case 2: s_headlight = !s_headlight;
                car_net_send(s_headlight ? "/headlight/on" : "/headlight/off"); break;
        case 3: s_taillight = !s_taillight;
                car_net_send(s_taillight ? "/taillight/on" : "/taillight/off"); break;
    }
    s_flash_on = true;
    reflect_btn(id);
    update_car();
}

static void flash_timer_cb(lv_timer_t *t) {
    if (s_blinkerL || s_blinkerR) {
        s_flash_on = !s_flash_on;
        update_car();
    }
}

static void clock_timer_cb(lv_timer_t *t) {
    if (++s_min >= 60) { s_min = 0; if (++s_hour >= 24) s_hour = 0; }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", s_hour, s_min);
    lv_label_set_text(lbl_time, buf);
}

static void status_timer_cb(lv_timer_t *t) {
    bool w = car_net_wifi_connected();
    bool c = car_net_car_linked();
    uint32_t col;
    const char *msg;
    if (!w)       { col = C_RED;   msg = "OFFLINE  -  RECONNECTING"; }
    else if (!c)  { col = C_AMBER; msg = "CONNECTED  -  FINDING CAR"; }
    else          { col = C_GREEN; msg = "ONLINE  -  CAR LINKED"; }
    lv_label_set_text(lbl_status, msg);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(col), 0);
    lv_obj_set_style_bg_color(dot_status, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(col), 0);
}

// =========================== builders ======================================
static void init_styles(void) {
    lv_style_init(&st_btn);
    lv_style_set_radius(&st_btn, 14);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_bg_color(&st_btn, lv_color_hex(C_PANEL));
    lv_style_set_border_width(&st_btn, 2);
    lv_style_set_border_color(&st_btn, lv_color_hex(C_BORDER));
    lv_style_set_text_color(&st_btn, lv_color_hex(C_MUTED));
    lv_style_set_text_font(&st_btn, &lv_font_montserrat_20);
    lv_style_set_shadow_width(&st_btn, 0);

    struct { lv_style_t *st; uint32_t fill, accent; } chk[] = {
        { &st_btn_amber, C_AMBER_D, C_AMBER },
        { &st_btn_cyan,  C_CYAN_D,  C_CYAN  },
        { &st_btn_red,   C_RED_D,   C_RED   },
    };
    for (int i = 0; i < 3; i++) {
        lv_style_init(chk[i].st);
        lv_style_set_bg_color(chk[i].st, lv_color_hex(chk[i].fill));
        lv_style_set_border_color(chk[i].st, lv_color_hex(chk[i].accent));
        lv_style_set_text_color(chk[i].st, lv_color_hex(C_TEXT));
        lv_style_set_shadow_color(chk[i].st, lv_color_hex(chk[i].accent));
        lv_style_set_shadow_width(chk[i].st, 18);
        lv_style_set_shadow_opa(chk[i].st, LV_OPA_40);
    }
}

static void build_status_bar(lv_obj_t *parent) {
    lbl_time = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(lbl_time, "21:12");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_LEFT, 14, 10);

    lv_obj_t *date = lv_label_create(parent);
    lv_obj_set_style_text_font(date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date, lv_color_hex(C_MUTED), 0);
    lv_label_set_text(date, "SAT  JUN 6");
    lv_obj_align(date, LV_ALIGN_TOP_LEFT, 86, 15);

    lbl_wifi = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(C_MUTED), 0);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(lbl_wifi, LV_ALIGN_TOP_RIGHT, -52, 13);

    lv_obj_t *batt = lv_label_create(parent);
    lv_obj_set_style_text_color(batt, lv_color_hex(C_GREEN), 0);
    lv_label_set_text(batt, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(batt, LV_ALIGN_TOP_RIGHT, -14, 13);

    lv_obj_t *divider = make_box(parent, 0, 38, 320, 1, 0, C_BORDER);
    (void)divider;
}

static void build_header(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(title, "AKKU TESLA");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

    dot_status = make_box(parent, 0, 0, 8, 8, 4, C_AMBER);
    lv_obj_align(dot_status, LV_ALIGN_TOP_MID, -96, 86);

    lbl_status = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(C_AMBER), 0);
    lv_label_set_text(lbl_status, "CONNECTING...");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 6, 83);
}

// Top-down car illustration built from positioned LVGL objects.
static void build_car(lv_obj_t *parent) {
    lv_obj_t *cv = lv_obj_create(parent);
    lv_obj_remove_style_all(cv);
    lv_obj_set_size(cv, 320, 232);
    lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);

    // Wheels first (so the body overlaps them)
    make_box(cv, 94,  42, 12, 34, 6, C_TIRE);
    make_box(cv, 214, 42, 12, 34, 6, C_TIRE);
    make_box(cv, 94,  156, 12, 34, 6, C_TIRE);
    make_box(cv, 214, 156, 12, 34, 6, C_TIRE);

    // Body with vertical silver gradient + drop shadow for depth
    lv_obj_t *body = make_box(cv, 102, 10, 116, 210, 38, C_SILVER);
    lv_obj_set_style_bg_grad_color(body, lv_color_hex(C_SILVER2), 0);
    lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_color(body, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(body, 24, 0);
    lv_obj_set_style_shadow_opa(body, LV_OPA_50, 0);

    // Glass: windshield, roof, rear window
    make_box(cv, 120, 56,  80, 44, 14, C_GLASS);
    make_box(cv, 118, 104, 84, 64, 16, C_ROOF);
    make_box(cv, 120, 172, 80, 30, 12, C_GLASS);

    // Headlight strips (front)
    car_headL = make_box(cv, 116, 18, 32, 9, 4, C_SOCKET);
    car_headR = make_box(cv, 172, 18, 32, 9, 4, C_SOCKET);

    // Taillight bar (rear)
    car_tail = make_box(cv, 120, 202, 80, 8, 4, C_SOCKET);

    // Blinkers (circles) at the four corners
    int bx[4] = {104, 202, 104, 202};
    int by[4] = {22, 22, 188, 188};
    for (int i = 0; i < 4; i++) {
        car_blink[i] = make_box(cv, bx[i], by[i], 14, 14, LV_RADIUS_CIRCLE, C_SOCKET);
    }

    update_car();
}

static void build_buttons(lv_obj_t *parent) {
    const char *labels[4] = { "BLINKER L", "BLINKER R", "HEADLIGHT", "TAILLIGHT" };
    lv_style_t *chk[4] = { &st_btn_amber, &st_btn_amber, &st_btn_cyan, &st_btn_red };
    int bx[4] = { 16, 166, 16, 166 };
    int by[4] = { 350, 350, 412, 412 };

    for (int i = 0; i < 4; i++) {
        btn[i] = lv_btn_create(parent);
        lv_obj_remove_style_all(btn[i]);
        lv_obj_add_style(btn[i], &st_btn, 0);
        lv_obj_add_style(btn[i], chk[i], LV_STATE_CHECKED);
        lv_obj_set_size(btn[i], 138, 52);
        lv_obj_set_pos(btn[i], bx[i], by[i]);
        lv_obj_add_event_cb(btn[i], btn_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = lv_label_create(btn[i]);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
    }
}

// =========================== entry =========================================
void car_ui_init(void) {
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG1), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(C_BG2), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    init_styles();
    build_status_bar(scr);
    build_header(scr);
    build_car(scr);
    build_buttons(scr);

    lv_timer_create(flash_timer_cb,  400, NULL);
    lv_timer_create(status_timer_cb, 1000, NULL);
    lv_timer_create(clock_timer_cb,  60000, NULL);

    lvgl_port_unlock();
}
