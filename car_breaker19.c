#include <furi.h>
#include <furi_hal.h>
#include <furi/core/kernel.h>
#include <furi/core/timer.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/dialog_ex.h>
#include <input/input.h>
#include <lib/subghz/subghz_worker.h>
#include <toolbox/crc32_calc.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <datetime/datetime.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "scenes/car_breaker19_scene_about.h"

#define TAG "CarBreaker19"

#define CAR_BREAKER_MAX_PULSES 512
#define CAR_BREAKER_MAX_FRAMES 64
#define CAR_BREAKER_MIN_PULSES 32
#define CAR_BREAKER_PACKET_GAP_US 4000
#define CAR_BREAKER_MAX_PULSE_US 30000
#define CAR_BREAKER_ROLLING_WINDOW_MS 2500
#define CAR_BREAKER_ROLLBACK_GAP_MS 8000
#define CAR_BREAKER_STORAGE_DIR "/ext/subghz/car_breaker19"
#define CAR_BREAKER_STATUS_MSG_LEN 64
#define KEY_DEBOUNCE_MS 300
#define CAR_BREAKER_HOP_INTERVAL_MS 500
#define CAR_BREAKER_PRESET_HOPPING 0

typedef struct CarBreakerApp CarBreakerApp;

typedef struct {
    uint32_t duration;
    uint8_t level;
} CarBreakerPulse;

typedef struct {
    uint32_t timestamp_ms;
    uint32_t hash;
    uint16_t pulse_count;
    uint16_t approx_bits;
    uint32_t duration_us;
    bool car_reacted;
    uint8_t preset_index;
} CarBreakerFrame;

typedef struct {
    CarBreakerFrame frames[CAR_BREAKER_MAX_FRAMES];
    size_t count;
} CarBreakerSession;

typedef struct {
    bool static_code_detected;
    uint32_t static_hash;
    uint32_t static_first_ms;
    uint32_t static_second_ms;
    bool rollback_detected;
    uint32_t rollback_hash;
    uint32_t rollback_gap_ms;
    bool rolling_pwn_detected;
    uint32_t rolling_window_ms;
} CarBreakerAnalysis;

typedef struct {
    const char* name;
    uint32_t frequency;
    const uint8_t* regs;
    size_t regs_size;
    const char* description;
} CarBreakerPreset;

typedef struct {
    CarBreakerApp* app;
    uint8_t ticker;
} CarBreakerViewModel;

static const uint8_t car_breaker_preset_hnd1[] = {
    0x02, 0x0D, 0x0B, 0x06, 0x08, 0x32, 0x07, 0x04, 0x14, 0x00, 0x13, 0x02,
    0x12, 0x04, 0x11, 0x36, 0x10, 0x69, 0x15, 0x32, 0x18, 0x18, 0x19, 0x16,
    0x1D, 0x91, 0x1C, 0x00, 0x1B, 0x07, 0x20, 0xFB, 0x22, 0x10, 0x21, 0x56,
    0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t car_breaker_preset_hnd2[] = {
    0x02, 0x0D, 0x0B, 0x06, 0x08, 0x32, 0x07, 0x04, 0x14, 0x00, 0x13, 0x02,
    0x12, 0x07, 0x11, 0x36, 0x10, 0xE9, 0x15, 0x32, 0x18, 0x18, 0x19, 0x16,
    0x1D, 0x92, 0x1C, 0x40, 0x1B, 0x03, 0x20, 0xFB, 0x22, 0x10, 0x21, 0x56,
    0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const CarBreakerPreset car_breaker_presets[] = {
    {
        .name = "Hopping",
        .frequency = 0,
        .regs = NULL,
        .regs_size = 0,
        .description = "Cycle all frequencies",
    },
    {
        .name = "HND-1 433.92",
        .frequency = 433920000,
        .regs = car_breaker_preset_hnd1,
        .regs_size = sizeof(car_breaker_preset_hnd1),
        .description = "Legacy BCM, wide filter",
    },
    {
        .name = "HND-2 433.92",
        .frequency = 433920000,
        .regs = car_breaker_preset_hnd2,
        .regs_size = sizeof(car_breaker_preset_hnd2),
        .description = "Modern BCM, narrow filter",
    },
    {
        .name = "HND-1 315",
        .frequency = 315000000,
        .regs = car_breaker_preset_hnd1,
        .regs_size = sizeof(car_breaker_preset_hnd1),
        .description = "Legacy BCM (NA/JPN)",
    },
    {
        .name = "HND-2 315",
        .frequency = 315000000,
        .regs = car_breaker_preset_hnd2,
        .regs_size = sizeof(car_breaker_preset_hnd2),
        .description = "Modern BCM (NA/JPN)",
    },
};

#define CAR_BREAKER_PRESETS_COUNT (COUNT_OF(car_breaker_presets))
#define CAR_BREAKER_REAL_PRESETS_START 1
#define CAR_BREAKER_REAL_PRESETS_COUNT (CAR_BREAKER_PRESETS_COUNT - 1)

typedef enum {
    CarBreakerViewSplash,
    CarBreakerViewMenu,
    CarBreakerViewConfig,
    CarBreakerViewCapture,
    CarBreakerViewReport,
    CarBreakerViewAbout,
} CarBreakerView;

typedef enum {
    CarBreakerMenuStart,
    CarBreakerMenuReport,
    CarBreakerMenuConfig,
    CarBreakerMenuExport,
    CarBreakerMenuReset,
    CarBreakerMenuAbout,
} CarBreakerMenuIndex;

struct CarBreakerApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    VariableItemList* config_list;
    DialogEx* dialog_ex;
    View* capture_view;
    View* report_view;
    View* about_view;
    NotificationApp* notifications;

    FuriMutex* lock;
    SubGhzWorker* worker;
    FuriTimer* ui_timer;
    FuriTimer* hop_timer;

    CarBreakerSession session;
    CarBreakerAnalysis analysis;
    CarBreakerPulse pulses[CAR_BREAKER_MAX_PULSES];
    uint16_t pulse_count;

    uint8_t preset_index;
    uint8_t current_hop_index;
    bool capturing;
    bool capture_dirty;
    bool overrun;
    bool is_view_transitioning;
    uint32_t last_key_event_time;
    char status_message[CAR_BREAKER_STATUS_MSG_LEN];
    CarBreakerView active_view;
};

static bool car_breaker_is_key_debounced(CarBreakerApp* app) {
    if(!app) return false;
    uint32_t current_time = furi_get_tick();
    if(current_time - app->last_key_event_time < KEY_DEBOUNCE_MS) {
        return false;
    }
    app->last_key_event_time = current_time;
    return true;
}

static void car_breaker_safe_switch_view(CarBreakerApp* app, CarBreakerView view) {
    if(!app || !app->view_dispatcher) return;
    if(app->is_view_transitioning) return;

    app->is_view_transitioning = true;
    app->active_view = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
    app->is_view_transitioning = false;
}

static void car_breaker_update_status(CarBreakerApp* app, const char* text) {
    furi_check(app);
    if(!text) return;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    snprintf(app->status_message, CAR_BREAKER_STATUS_MSG_LEN, "%s", text);
    app->capture_dirty = true;
    furi_mutex_release(app->lock);
}

static uint32_t car_breaker_hash_pulses(const CarBreakerPulse* pulses, size_t count) {
    uint32_t crc = 0;
    for(size_t i = 0; i < count; i++) {
        crc = crc32_calc_buffer(crc, &pulses[i].duration, sizeof(pulses[i].duration));
        crc = crc32_calc_buffer(crc, &pulses[i].level, sizeof(pulses[i].level));
    }
    return crc;
}

static void car_breaker_reset_session_locked(CarBreakerApp* app) {
    app->session.count = 0;
    memset(&app->analysis, 0, sizeof(app->analysis));
    app->pulse_count = 0;
    app->capture_dirty = true;
}

static void car_breaker_recompute_analysis_locked(CarBreakerApp* app) {
    CarBreakerAnalysis updated = {0};
    const CarBreakerSession* session = &app->session;

    for(size_t idx = 0; idx < session->count; idx++) {
        const CarBreakerFrame* frame = &session->frames[idx];
        if(idx >= 4 && !updated.rolling_pwn_detected) {
            const CarBreakerFrame* start = &session->frames[idx - 4];
            uint32_t window = frame->timestamp_ms - start->timestamp_ms;
            if(window <= CAR_BREAKER_ROLLING_WINDOW_MS) {
                updated.rolling_pwn_detected = true;
                updated.rolling_window_ms = window;
            }
        }
        for(size_t older_idx = 0; older_idx < idx; older_idx++) {
            const CarBreakerFrame* older = &session->frames[older_idx];
            if(older->hash != frame->hash) continue;
            uint32_t gap = frame->timestamp_ms - older->timestamp_ms;
            if(!updated.static_code_detected) {
                updated.static_code_detected = true;
                updated.static_hash = frame->hash;
                updated.static_first_ms = older->timestamp_ms;
                updated.static_second_ms = frame->timestamp_ms;
            }
            bool reaction_pair =
                older->car_reacted && frame->car_reacted && (idx - older_idx) >= 2;
            if(!updated.rollback_detected &&
               (gap >= CAR_BREAKER_ROLLBACK_GAP_MS || reaction_pair)) {
                updated.rollback_detected = true;
                updated.rollback_hash = frame->hash;
                updated.rollback_gap_ms = gap;
            }
        }
    }

    app->analysis = updated;
}

static void car_breaker_finalize_frame_locked(CarBreakerApp* app) {
    if(app->pulse_count < CAR_BREAKER_MIN_PULSES) {
        app->pulse_count = 0;
        return;
    }

    CarBreakerSession* session = &app->session;
    if(session->count >= CAR_BREAKER_MAX_FRAMES) {
        memmove(
            &session->frames[0],
            &session->frames[1],
            (CAR_BREAKER_MAX_FRAMES - 1) * sizeof(CarBreakerFrame));
        session->count = CAR_BREAKER_MAX_FRAMES - 1;
    }

    CarBreakerFrame* frame = &session->frames[session->count++];
    frame->timestamp_ms = furi_get_tick();
    frame->pulse_count = app->pulse_count;
    frame->hash = car_breaker_hash_pulses(app->pulses, app->pulse_count);
    frame->preset_index = app->preset_index;
    frame->car_reacted = false;

    uint64_t duration_sum = 0;
    for(size_t i = 0; i < app->pulse_count; i++) {
        duration_sum += app->pulses[i].duration;
    }
    if(duration_sum > UINT32_MAX) duration_sum = UINT32_MAX;
    frame->duration_us = (uint32_t)duration_sum;
    frame->approx_bits = (uint16_t)(duration_sum / 65);

    app->pulse_count = 0;
    app->capture_dirty = true;
    car_breaker_recompute_analysis_locked(app);
}

static void car_breaker_worker_overrun_callback(void* context) {
    CarBreakerApp* app = context;
    app->overrun = true;
}

static void car_breaker_worker_pair_callback(void* context, bool level, uint32_t duration) {
    CarBreakerApp* app = context;
    if(!app->capturing) return;

    if(duration > CAR_BREAKER_MAX_PULSE_US) duration = CAR_BREAKER_MAX_PULSE_US;

    furi_mutex_acquire(app->lock, FuriWaitForever);

    if(app->pulse_count >= CAR_BREAKER_MAX_PULSES) {
        car_breaker_finalize_frame_locked(app);
    }

    if(app->pulse_count < CAR_BREAKER_MAX_PULSES) {
        CarBreakerPulse* pulse = &app->pulses[app->pulse_count++];
        pulse->duration = duration;
        pulse->level = level ? 1 : 0;
    }

    if(!level && duration >= CAR_BREAKER_PACKET_GAP_US) {
        car_breaker_finalize_frame_locked(app);
    }

    furi_mutex_release(app->lock);
}

static void car_breaker_ui_timer_callback(void* context) {
    CarBreakerApp* app = context;
    bool dirty = false;

    furi_mutex_acquire(app->lock, FuriWaitForever);
    dirty = app->capture_dirty;
    app->capture_dirty = false;
    furi_mutex_release(app->lock);

    if(dirty && app->active_view == CarBreakerViewCapture) {
        with_view_model(
            app->capture_view, CarBreakerViewModel * model, { model->ticker++; }, true);
    }
    if(dirty && app->active_view == CarBreakerViewReport) {
        with_view_model(
            app->report_view, CarBreakerViewModel * model, { model->ticker++; }, true);
    }
}

static void car_breaker_apply_preset(CarBreakerApp* app, uint8_t preset_idx) {
    if(preset_idx < CAR_BREAKER_REAL_PRESETS_START || preset_idx >= CAR_BREAKER_PRESETS_COUNT) {
        return;
    }

    if(subghz_worker_is_running(app->worker)) {
        subghz_worker_stop(app->worker);
        furi_hal_subghz_stop_async_rx();
    }
    furi_hal_subghz_idle();

    const CarBreakerPreset* preset = &car_breaker_presets[preset_idx];
    furi_hal_subghz_load_custom_preset(preset->regs);
    furi_hal_subghz_set_frequency_and_path(preset->frequency);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();

    furi_hal_subghz_start_async_rx(subghz_worker_rx_callback, app->worker);
    subghz_worker_start(app->worker);

    app->current_hop_index = preset_idx;
    app->capture_dirty = true;
}

static void car_breaker_hop_timer_callback(void* context) {
    CarBreakerApp* app = context;
    if(!app->capturing) return;

    uint8_t next_idx = app->current_hop_index + 1;
    if(next_idx >= CAR_BREAKER_PRESETS_COUNT) {
        next_idx = CAR_BREAKER_REAL_PRESETS_START;
    }

    car_breaker_apply_preset(app, next_idx);
}

static void car_breaker_start_capture(CarBreakerApp* app) {
    if(app->capturing) return;

    car_breaker_update_status(app, "Listening...");

    furi_hal_subghz_reset();
    furi_hal_subghz_idle();

    subghz_worker_set_context(app->worker, app);
    subghz_worker_set_pair_callback(app->worker, car_breaker_worker_pair_callback);
    subghz_worker_set_overrun_callback(app->worker, car_breaker_worker_overrun_callback);

    if(app->preset_index == CAR_BREAKER_PRESET_HOPPING) {
        app->current_hop_index = CAR_BREAKER_REAL_PRESETS_START;
        car_breaker_apply_preset(app, app->current_hop_index);

        if(!app->hop_timer) {
            app->hop_timer =
                furi_timer_alloc(car_breaker_hop_timer_callback, FuriTimerTypePeriodic, app);
        }
        furi_timer_start(app->hop_timer, furi_ms_to_ticks(CAR_BREAKER_HOP_INTERVAL_MS));
    } else {
        app->current_hop_index = app->preset_index;
        const CarBreakerPreset* preset = &car_breaker_presets[app->preset_index];
        furi_hal_subghz_load_custom_preset(preset->regs);
        furi_hal_subghz_set_frequency_and_path(preset->frequency);
        furi_hal_subghz_flush_rx();
        furi_hal_subghz_rx();

        furi_hal_subghz_start_async_rx(subghz_worker_rx_callback, app->worker);
        subghz_worker_start(app->worker);
    }

    app->pulse_count = 0;
    app->capturing = true;
    app->overrun = false;

    if(!app->ui_timer) {
        app->ui_timer =
            furi_timer_alloc(car_breaker_ui_timer_callback, FuriTimerTypePeriodic, app);
    }
    furi_timer_start(app->ui_timer, furi_ms_to_ticks(250));
}

static void car_breaker_stop_capture(CarBreakerApp* app) {
    if(!app->capturing) return;

    if(app->hop_timer) {
        furi_timer_stop(app->hop_timer);
    }

    if(subghz_worker_is_running(app->worker)) {
        subghz_worker_stop(app->worker);
        furi_hal_subghz_stop_async_rx();
    }
    furi_hal_subghz_idle();

    app->capturing = false;
    if(app->ui_timer) {
        furi_timer_stop(app->ui_timer);
    }
    car_breaker_update_status(app, "Capture paused");
}

static void car_breaker_toggle_latest_reaction(CarBreakerApp* app) {
    furi_mutex_acquire(app->lock, FuriWaitForever);
    if(app->session.count == 0) {
        furi_mutex_release(app->lock);
        car_breaker_update_status(app, "No frames yet");
        return;
    }
    CarBreakerFrame* frame = &app->session.frames[app->session.count - 1];
    frame->car_reacted = !frame->car_reacted;
    car_breaker_recompute_analysis_locked(app);
    app->capture_dirty = true;
    furi_mutex_release(app->lock);

    car_breaker_update_status(
        app, frame->car_reacted ? "Marked: car reacted" : "Marked: no reaction");
}

static void car_breaker_reset_session(CarBreakerApp* app) {
    furi_mutex_acquire(app->lock, FuriWaitForever);
    car_breaker_reset_session_locked(app);
    furi_mutex_release(app->lock);
    car_breaker_update_status(app, "Session cleared");
}

static void car_breaker_capture_view_draw(Canvas* canvas, void* model) {
    furi_check(model);
    CarBreakerViewModel* view_model = model;
    CarBreakerApp* app = view_model->app;

    size_t frame_count = 0;
    CarBreakerFrame last_frame = {0};
    CarBreakerAnalysis analysis = {0};
    bool overrun = false;
    bool capturing = false;

    furi_mutex_acquire(app->lock, FuriWaitForever);
    frame_count = app->session.count;
    if(frame_count > 0) {
        last_frame = app->session.frames[frame_count - 1];
    }
    analysis = app->analysis;
    overrun = app->overrun;
    capturing = app->capturing;
    furi_mutex_release(app->lock);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    char header[40];
    if(app->preset_index == CAR_BREAKER_PRESET_HOPPING) {
        snprintf(
            header,
            sizeof(header),
            "%s: Hop %s",
            capturing ? "Capturing" : "Paused",
            car_breaker_presets[app->current_hop_index].name);
    } else {
        snprintf(
            header,
            sizeof(header),
            "%s: %s",
            capturing ? "Capturing" : "Paused",
            car_breaker_presets[app->preset_index].name);
    }
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, header);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 4, 14, 120, 36);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 4, 14, 120, 36);

    canvas_set_font(canvas, FontSecondary);

    char line[40];
    snprintf(line, sizeof(line), "Frames: %u", (unsigned)frame_count);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, line);

    if(frame_count > 0) {
        snprintf(line, sizeof(line), "Last: %08lX", (unsigned long)last_frame.hash);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, line);
    } else {
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, "Waiting for signal...");
    }

    snprintf(
        line,
        sizeof(line),
        "S:%s  RB:%s  RP:%s",
        analysis.static_code_detected ? "!" : "OK",
        analysis.rollback_detected ? "!" : "OK",
        analysis.rolling_pwn_detected ? "!" : "OK");
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, line);

    if(overrun) {
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignTop, "RX Overrun!");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignTop, "OK:Mark  BACK:Stop");
}

static bool car_breaker_capture_view_input(InputEvent* event, void* context) {
    CarBreakerApp* app = context;
    if(event->type != InputTypeShort) return false;

    if(!car_breaker_is_key_debounced(app)) return true;

    if(event->key == InputKeyBack) {
        car_breaker_stop_capture(app);
        car_breaker_safe_switch_view(app, CarBreakerViewMenu);
        return true;
    }

    if(event->key == InputKeyOk) {
        car_breaker_toggle_latest_reaction(app);
        return true;
    }

    return false;
}

static void car_breaker_report_view_draw(Canvas* canvas, void* model) {
    furi_check(model);
    CarBreakerViewModel* view_model = model;
    CarBreakerApp* app = view_model->app;

    CarBreakerAnalysis analysis = {0};
    size_t frames = 0;

    furi_mutex_acquire(app->lock, FuriWaitForever);
    analysis = app->analysis;
    frames = app->session.count;
    furi_mutex_release(app->lock);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Analysis Report");

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 4, 14, 120, 38);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 4, 14, 120, 38);

    canvas_set_font(canvas, FontSecondary);

    char line[48];

    snprintf(line, sizeof(line), "Frames: %u", (unsigned)frames);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, line);

    snprintf(
        line, sizeof(line), "Static: %s", analysis.static_code_detected ? "DETECTED" : "OK");
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, line);

    snprintf(
        line, sizeof(line), "RollBack: %s", analysis.rollback_detected ? "SUSPECT" : "OK");
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, line);

    snprintf(
        line, sizeof(line), "Rolling-Pwn: %s", analysis.rolling_pwn_detected ? "DETECTED" : "OK");
    canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignTop, line);

    canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignTop, "BACK to return");
}

static bool car_breaker_report_view_input(InputEvent* event, void* context) {
    CarBreakerApp* app = context;
    if(event->type != InputTypeShort) return false;

    if(!car_breaker_is_key_debounced(app)) return true;

    if(event->key == InputKeyOk || event->key == InputKeyBack) {
        car_breaker_safe_switch_view(app, CarBreakerViewMenu);
        return true;
    }
    return false;
}

static uint32_t car_breaker_splash_previous_callback(void* context) {
    UNUSED(context);
    return CarBreakerViewSplash;
}

static uint32_t car_breaker_menu_previous_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t car_breaker_view_previous_callback(void* context) {
    UNUSED(context);
    return CarBreakerViewMenu;
}

static void car_breaker_config_preset_changed(VariableItem* item) {
    CarBreakerApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    if(index >= COUNT_OF(car_breaker_presets)) index = 0;
    app->preset_index = index;
    variable_item_set_current_value_text(item, car_breaker_presets[index].name);
    car_breaker_update_status(app, car_breaker_presets[index].description);
}

static bool car_breaker_save_session(CarBreakerApp* app) {
    bool result = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) return false;

    storage_common_mkdir(storage, CAR_BREAKER_STORAGE_DIR);

    DateTime datetime = {0};
    furi_hal_rtc_get_datetime(&datetime);

    char path[128];
    snprintf(
        path,
        sizeof(path),
        "%s/session_%04u%02u%02u_%02u%02u%02u.json",
        CAR_BREAKER_STORAGE_DIR,
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second);

    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* json = furi_string_alloc();
        furi_string_printf(
            json,
            "{\n  \"preset\": \"%s\",\n  \"frames\": [\n",
            car_breaker_presets[app->preset_index].name);

        furi_mutex_acquire(app->lock, FuriWaitForever);
        for(size_t i = 0; i < app->session.count; i++) {
            const CarBreakerFrame* frame = &app->session.frames[i];
            furi_string_cat_printf(
                json,
                "    {\"idx\": %u, \"hash\": \"%08lX\", \"duration_us\": %lu, \"reacted\": %s}%s\n",
                (unsigned)i,
                (unsigned long)frame->hash,
                (unsigned long)frame->duration_us,
                frame->car_reacted ? "true" : "false",
                (i + 1 == app->session.count) ? "" : ",");
        }
        furi_mutex_release(app->lock);

        furi_string_cat_str(json, "  ]\n}\n");
        storage_file_write(file, furi_string_get_cstr(json), furi_string_size(json));
        furi_string_free(json);
        storage_file_close(file);
        result = true;
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return result;
}

static void car_breaker_splash_callback(DialogExResult result, void* context) {
    CarBreakerApp* app = context;
    if(result == DialogExResultCenter) {
        car_breaker_safe_switch_view(app, CarBreakerViewMenu);
    }
}

static void car_breaker_submenu_callback(void* context, uint32_t index) {
    CarBreakerApp* app = context;

    if(!car_breaker_is_key_debounced(app)) return;

    switch(index) {
    case CarBreakerMenuStart:
        car_breaker_start_capture(app);
        car_breaker_safe_switch_view(app, CarBreakerViewCapture);
        break;
    case CarBreakerMenuReport:
        car_breaker_safe_switch_view(app, CarBreakerViewReport);
        break;
    case CarBreakerMenuConfig:
        car_breaker_safe_switch_view(app, CarBreakerViewConfig);
        break;
    case CarBreakerMenuExport:
        if(car_breaker_save_session(app)) {
            notification_message(app->notifications, &sequence_success);
            car_breaker_update_status(app, "Session exported");
        } else {
            notification_message(app->notifications, &sequence_error);
            car_breaker_update_status(app, "Export failed");
        }
        break;
    case CarBreakerMenuReset:
        car_breaker_reset_session(app);
        break;
    case CarBreakerMenuAbout:
        car_breaker_safe_switch_view(app, CarBreakerViewAbout);
        break;
    default:
        break;
    }
}

static CarBreakerApp* car_breaker_alloc(void) {
    CarBreakerApp* app = malloc(sizeof(CarBreakerApp));
    memset(app, 0, sizeof(CarBreakerApp));

    app->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    app->worker = subghz_worker_alloc();
    app->preset_index = 0;
    snprintf(app->status_message, sizeof(app->status_message), "Select preset");

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->dialog_ex = dialog_ex_alloc();
    dialog_ex_set_header(app->dialog_ex, "Car Breaker 19", 64, 3, AlignCenter, AlignTop);
    dialog_ex_set_text(
        app->dialog_ex,
        "Passive Honda RKE analyzer\nDetect Rolling-Pwn, RollBack,\nand static-code risks.",
        64,
        32,
        AlignCenter,
        AlignCenter);
    dialog_ex_set_center_button_text(app->dialog_ex, "Continue");
    dialog_ex_set_context(app->dialog_ex, app);
    dialog_ex_set_result_callback(app->dialog_ex, car_breaker_splash_callback);
    view_set_previous_callback(dialog_ex_get_view(app->dialog_ex), car_breaker_splash_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, CarBreakerViewSplash, dialog_ex_get_view(app->dialog_ex));

    app->submenu = submenu_alloc();
    submenu_set_header(app->submenu, "Car Breaker 19");
    submenu_add_item(
        app->submenu, "Start Capture", CarBreakerMenuStart, car_breaker_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Report", CarBreakerMenuReport, car_breaker_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Config", CarBreakerMenuConfig, car_breaker_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Export Session", CarBreakerMenuExport, car_breaker_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Reset Session", CarBreakerMenuReset, car_breaker_submenu_callback, app);
    submenu_add_item(
        app->submenu, "About", CarBreakerMenuAbout, car_breaker_submenu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), car_breaker_menu_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, CarBreakerViewMenu, submenu_get_view(app->submenu));

    app->config_list = variable_item_list_alloc();
    variable_item_list_set_header(app->config_list, "Capture Options");
    VariableItem* preset_item = variable_item_list_add(
        app->config_list,
        "Preset",
        COUNT_OF(car_breaker_presets),
        car_breaker_config_preset_changed,
        app);
    variable_item_set_current_value_index(preset_item, app->preset_index);
    variable_item_set_current_value_text(preset_item, car_breaker_presets[app->preset_index].name);
    view_set_previous_callback(
        variable_item_list_get_view(app->config_list), car_breaker_view_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, CarBreakerViewConfig, variable_item_list_get_view(app->config_list));

    app->capture_view = view_alloc();
    view_set_context(app->capture_view, app);
    view_set_draw_callback(app->capture_view, car_breaker_capture_view_draw);
    view_set_input_callback(app->capture_view, car_breaker_capture_view_input);
    view_set_previous_callback(app->capture_view, car_breaker_view_previous_callback);
    view_allocate_model(app->capture_view, ViewModelTypeLockFree, sizeof(CarBreakerViewModel));
    with_view_model(
        app->capture_view,
        CarBreakerViewModel * model,
        {
            model->app = app;
            model->ticker = 0;
        },
        false);
    view_dispatcher_add_view(app->view_dispatcher, CarBreakerViewCapture, app->capture_view);

    app->report_view = view_alloc();
    view_set_context(app->report_view, app);
    view_set_draw_callback(app->report_view, car_breaker_report_view_draw);
    view_set_input_callback(app->report_view, car_breaker_report_view_input);
    view_set_previous_callback(app->report_view, car_breaker_view_previous_callback);
    view_allocate_model(app->report_view, ViewModelTypeLockFree, sizeof(CarBreakerViewModel));
    with_view_model(
        app->report_view,
        CarBreakerViewModel * model,
        {
            model->app = app;
            model->ticker = 0;
        },
        false);
    view_dispatcher_add_view(app->view_dispatcher, CarBreakerViewReport, app->report_view);

    app->about_view = car_breaker19_scene_about_alloc();
    view_set_previous_callback(app->about_view, car_breaker_view_previous_callback);
    view_dispatcher_add_view(app->view_dispatcher, CarBreakerViewAbout, app->about_view);

    return app;
}

static void car_breaker_free(CarBreakerApp* app) {
    if(!app) return;

    car_breaker_stop_capture(app);

    if(app->ui_timer) {
        furi_timer_free(app->ui_timer);
    }

    if(app->hop_timer) {
        furi_timer_free(app->hop_timer);
    }

    view_dispatcher_remove_view(app->view_dispatcher, CarBreakerViewAbout);
    car_breaker19_scene_about_free(app->about_view);

    view_dispatcher_remove_view(app->view_dispatcher, CarBreakerViewReport);
    view_free(app->report_view);

    view_dispatcher_remove_view(app->view_dispatcher, CarBreakerViewCapture);
    view_free(app->capture_view);

    view_dispatcher_remove_view(app->view_dispatcher, CarBreakerViewConfig);
    variable_item_list_free(app->config_list);

    view_dispatcher_remove_view(app->view_dispatcher, CarBreakerViewMenu);
    submenu_free(app->submenu);

    view_dispatcher_remove_view(app->view_dispatcher, CarBreakerViewSplash);
    dialog_ex_free(app->dialog_ex);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    subghz_worker_free(app->worker);
    furi_mutex_free(app->lock);

    free(app);
}

static void car_breaker_run(CarBreakerApp* app) {
    car_breaker_safe_switch_view(app, CarBreakerViewSplash);
    view_dispatcher_run(app->view_dispatcher);
}

int32_t car_breaker19_app(void* arg) {
    UNUSED(arg);
    CarBreakerApp* app = car_breaker_alloc();
    car_breaker_run(app);
    car_breaker_free(app);
    return 0;
}
