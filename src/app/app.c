#include <stdbool.h>
#include <SDL.h>
#include <assert.h>
#include "lvgl/lv_sdl_drv_input.h"
#include "lvgl/lv_disp_drv_app.h"
#include "lvgl/util/lv_app_utils.h"

#include "app.h"
#include "config.h"

#include "logging.h"
#include "logging_ext_sdl.h"
#include "logging_ext_ss4s.h"
#include "backend/backend_root.h"
#include "stream/session.h"
#include "ui/root.h"
#include "util/bus.h"
#include "util/user_event.h"
#include "util/i18n.h"

#include "ss4s_modules.h"
#include "ss4s.h"
#include "stream/session_events.h"
#include "ui/fatal_error.h"
#include "app_error.h"
#include "app_session.h"
#include "stream/embed_wrapper.h"

PCONFIGURATION app_configuration = NULL;

static void quit_confirm_cb(lv_event_t *e);

static void libs_init(app_t *app, int argc, char *argv[]);

#if TARGET_WEBOS

static int app_diag_event_watch(void *userdata, SDL_Event *event);

static void app_diag_log_sdl_event(const SDL_Event *event);

static const char *app_diag_sdl_event_name(Uint32 type);

#endif

app_t *global = NULL;


int app_init(app_t *app, app_settings_loader *settings_loader, int argc, char *argv[]) {
    assert(settings_loader != NULL);
    memset(app, 0, sizeof(*app));
    commons_logging_init("moonlight");
    SDL_LogSetOutputFunction(commons_sdl_log, NULL);
    SDL_SetAssertionHandler(app_assertion_handler_abort, NULL);
    SDL_Init(0);
    commons_log_info("APP", "Start Moonlight. Version %s", APP_VERSION);
    settings_loader(&app->settings);
    app->main_thread_id = SDL_ThreadID();
    app->running = true;
    app->focused = false;
#if FEATURE_EMBEDDED_SHELL
    app->embed_version.major = -1;
#endif
    app_configuration = &app->settings;
    if (os_info_get(&app->os_info) == 0) {
        char *info_str = os_info_str(&app->os_info);
        commons_log_info("APP", "System: %s", info_str);
        free(info_str);
    }
    libs_init(app, argc, argv);

    app_init_locale();
    backend_init(&app->backend, app);

#if TARGET_WEBOS
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_SLEEP_TIME, "5000");
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_FREQUENCY, "60");
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_CALIBRATION_DISABLE, "true");
    SDL_SetHint(SDL_HINT_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES, "0x057e/0x0000");
    if (app->settings.syskey_capture) {
        SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_HOME, "true");
        SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_RIBBON, "false");
    }
#else
    if (app->settings.syskey_capture) {
        SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    }
#endif
    // DO not init video subsystem before NDL/LGNC initialization
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        commons_log_fatal("APP", "Failed to initialize SDL video subsystem: %s", SDL_GetError());
        return -1;
    }
    // This will occupy SDL_USEREVENT
    SDL_RegisterEvents(1);
    commons_log_info("APP", "UI locale: %s (%s)", i18n_locale(), locstr("[Localized Language]"));

    app_input_init(&app->input, app);

    app_ui_init(&app->ui, app);

#if TARGET_WEBOS
    SDL_AddEventWatch(app_diag_event_watch, app);
#endif

    global = app;

    SS4S_PostInit(argc, argv);
    return 0;
}

void app_deinit(app_t *app) {
#if TARGET_WEBOS
    SDL_DelEventWatch(app_diag_event_watch, app);
#endif
    app_bus_drain();
    app_session_destroy(app);
    app_ui_close(&app->ui);
    app_ui_deinit(&app->ui);
    app_set_keep_awake(app, false);
    app_input_deinit(&app->input);

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    backend_destroy(&app->backend);

    settings_save(&app->settings);
    settings_clear(&app->settings);
    free(app->settings.conf_dir);

#if FEATURE_INPUT_LIBCEC
    cec_sdl_deinit(&app->cec);
#endif
    SS4S_Quit();

    SS4S_ModulesListClear(&app->ss4s.modules);
    os_info_clear(&app->os_info);

    _lv_draw_mask_cleanup();

    SDL_Quit();

    commons_logging_deinit();
}

void app_run_loop(app_t *app) {
    app_process_events(app);
    lv_task_handler();
    SDL_Delay(1);
}

static int app_event_filter(void *userdata, SDL_Event *event) {
    app_t *app = userdata;
    switch (event->type) {
        case SDL_APP_WILLENTERBACKGROUND: {
            // Interrupt streaming because app will go to background
            if (app_ui_is_opened(&app->ui) && app->session != NULL) {
                session_interrupt(app->session, false, STREAMING_INTERRUPT_BACKGROUND);
            }
            break;
        }
        case SDL_APP_DIDENTERFOREGROUND: {
            lv_obj_invalidate(lv_scr_act());
            break;
        }
        case SDL_WINDOWEVENT: {
            switch (event->window.event) {
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    app->focused = true;
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    commons_log_debug("SDL", "Window event SDL_WINDOWEVENT_FOCUS_LOST");
#if !FEATURE_FORCE_FULLSCREEN
                    if (app_configuration->fullscreen && app_ui_is_opened(&app->ui) && app->session != NULL) {
                        // Interrupt streaming because app will go to background
                        session_interrupt(app->session, false, STREAMING_INTERRUPT_BACKGROUND);
                    }
#endif
                    app->focused = false;
                    break;
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    lv_app_display_resize(lv_disp_get_default(), event->window.data1, event->window.data2);
                    ui_display_size(&app->ui, (int) event->window.data1, (int) event->window.data2);
                    bus_pushevent(USER_SIZE_CHANGED, NULL, NULL);
                    break;
                case SDL_WINDOWEVENT_HIDDEN:
                    commons_log_debug("SDL", "Window event SDL_WINDOWEVENT_HIDDEN");
                    if (app_configuration->fullscreen && app_ui_is_opened(&app->ui) && app->session != NULL) {
                        // Interrupt streaming because app will go to background
                        session_interrupt(app->session, false, STREAMING_INTERRUPT_BACKGROUND);
                    }
                    break;
                case SDL_WINDOWEVENT_EXPOSED: {
                    lv_obj_invalidate(lv_scr_act());
                    break;
                }
                case SDL_WINDOWEVENT_CLOSE: {
                    if (app_ui_is_opened(&app->ui)) {
                        app_request_exit();
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
        case SDL_USEREVENT: {
            if (event->user.code == BUS_INT_EVENT_ACTION) {
                bus_actionfunc actionfn = event->user.data1;
                actionfn(event->user.data2);
            } else if (event->user.code == USER_INPUT_CONTROLLERDB_UPDATED) {
                app_input_handle_event(&app->input, event);
            } else {
                bool handled = backend_dispatch_userevent(&app->backend, event->user.code, event->user.data1,
                                                          event->user.data2);
                handled = handled || ui_dispatch_userevent(app, event->user.code, event->user.data1, event->user.data2);
                if (!handled) {
                    if (event->user.code & USER_EVENT_FLAG_FREE_DATA1 && event->user.data1 != NULL) {
                        free(event->user.data1);
                    }
                    if (event->user.code & USER_EVENT_FLAG_FREE_DATA2 && event->user.data2 != NULL) {
                        free(event->user.data2);
                    }
                }
            }
            break;
        }
        case SDL_QUIT: {
            app_request_exit();
            break;
        }
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEREMAPPED: {
            app_input_handle_event(&app->input, event);
            if (app->session != NULL) {
                session_handle_input_event(app->session, event);
                return 0;
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL:
        case SDL_CONTROLLERAXISMOTION:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADMOTION:
        case SDL_CONTROLLERTOUCHPADUP:
        case SDL_CONTROLLERSENSORUPDATE:
        case SDL_TEXTINPUT: {
            if (event->type == SDL_MOUSEMOTION) {
                bool updated = app_text_input_state_update(&app->ui.input);
                if (updated && !app->ui.input.text_input_active && app->session != NULL) {
                    session_screen_keyboard_closed(app->session);
                }
            }
            if (!app_ui_is_opened(&app->ui) && app->session != NULL) {
                session_handle_input_event(app->session, event);
                return 0;
            }
            return 1;
        }
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION: {
            if (app->session != NULL) {
                session_handle_input_event(app->session, event);
                return 0;
            }
            return 1;
        }
        default:
            if (event->type == USER_REMOTEBUTTONEVENT) {
                return 1;
            }
            return 0;
    }
    return 0;
}

void app_process_events(app_t *app) {
    SDL_PumpEvents();
    SDL_FilterEvents(app_event_filter, app);
}

#if TARGET_WEBOS

static int app_diag_event_watch(void *userdata, SDL_Event *event) {
    app_t *app = userdata;
    if (app != NULL && app->session != NULL) {
        app_diag_log_sdl_event(event);
    }
    return 0;
}

static void app_diag_log_sdl_event(const SDL_Event *event) {
    switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) scancode=%d (%s) keycode=%d (%s) state=%s mod=0x%x repeat=%d",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->key.keysym.scancode,
                        SDL_GetScancodeName(event->key.keysym.scancode),
                        event->key.keysym.sym,
                        SDL_GetKeyName(event->key.keysym.sym),
                        event->key.state == SDL_PRESSED ? "DOWN" : "UP",
                        event->key.keysym.mod,
                        event->key.repeat);
            break;
        case SDL_TEXTINPUT:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) text=\"%s\"",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->text.text);
            break;
        case SDL_MOUSEMOTION:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%u x=%d y=%d xrel=%d yrel=%d",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->motion.which,
                        event->motion.x,
                        event->motion.y,
                        event->motion.xrel,
                        event->motion.yrel);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%u button=%u state=%s clicks=%u x=%d y=%d",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->button.which,
                        event->button.button,
                        event->button.state == SDL_PRESSED ? "DOWN" : "UP",
                        event->button.clicks,
                        event->button.x,
                        event->button.y);
            break;
        case SDL_MOUSEWHEEL:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%u x=%d y=%d direction=%u",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->wheel.which,
                        event->wheel.x,
                        event->wheel.y,
                        event->wheel.direction);
            break;
        case SDL_CONTROLLERAXISMOTION:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%d axis=%u value=%d",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->caxis.which,
                        event->caxis.axis,
                        event->caxis.value);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%d button=%u state=%s",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->cbutton.which,
                        event->cbutton.button,
                        event->cbutton.state == SDL_PRESSED ? "DOWN" : "UP");
            break;
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADMOTION:
        case SDL_CONTROLLERTOUCHPADUP:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%d touchpad=%d finger=%d x=%f y=%f pressure=%f",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->ctouchpad.which,
                        event->ctouchpad.touchpad,
                        event->ctouchpad.finger,
                        event->ctouchpad.x,
                        event->ctouchpad.y,
                        event->ctouchpad.pressure);
            break;
        case SDL_CONTROLLERSENSORUPDATE:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%d sensor=%d",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->csensor.which,
                        event->csensor.sensor);
            break;
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEREMAPPED:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) which=%d",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->cdevice.which);
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) touchId=%lld fingerId=%lld x=%f y=%f pressure=%f",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        (long long)event->tfinger.touchId,
                        (long long)event->tfinger.fingerId,
                        event->tfinger.x,
                        event->tfinger.y,
                        event->tfinger.pressure);
            break;
        case SDL_USEREVENT:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s) code=%d data1=%p data2=%p",
                        event->type,
                        app_diag_sdl_event_name(event->type),
                        event->user.code,
                        event->user.data1,
                        event->user.data2);
            break;
        default:
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "MLTV_DIAG_SDL_EVENT type=%u (%s)",
                        event->type,
                        app_diag_sdl_event_name(event->type));
            break;
    }
}

static const char *app_diag_sdl_event_name(Uint32 type) {
    switch (type) {
        case SDL_KEYDOWN:
            return "SDL_KEYDOWN";
        case SDL_KEYUP:
            return "SDL_KEYUP";
        case SDL_TEXTINPUT:
            return "SDL_TEXTINPUT";
        case SDL_MOUSEMOTION:
            return "SDL_MOUSEMOTION";
        case SDL_MOUSEBUTTONDOWN:
            return "SDL_MOUSEBUTTONDOWN";
        case SDL_MOUSEBUTTONUP:
            return "SDL_MOUSEBUTTONUP";
        case SDL_MOUSEWHEEL:
            return "SDL_MOUSEWHEEL";
        case SDL_CONTROLLERAXISMOTION:
            return "SDL_CONTROLLERAXISMOTION";
        case SDL_CONTROLLERBUTTONDOWN:
            return "SDL_CONTROLLERBUTTONDOWN";
        case SDL_CONTROLLERBUTTONUP:
            return "SDL_CONTROLLERBUTTONUP";
        case SDL_CONTROLLERTOUCHPADDOWN:
            return "SDL_CONTROLLERTOUCHPADDOWN";
        case SDL_CONTROLLERTOUCHPADMOTION:
            return "SDL_CONTROLLERTOUCHPADMOTION";
        case SDL_CONTROLLERTOUCHPADUP:
            return "SDL_CONTROLLERTOUCHPADUP";
        case SDL_CONTROLLERSENSORUPDATE:
            return "SDL_CONTROLLERSENSORUPDATE";
        case SDL_JOYDEVICEADDED:
            return "SDL_JOYDEVICEADDED";
        case SDL_JOYDEVICEREMOVED:
            return "SDL_JOYDEVICEREMOVED";
        case SDL_CONTROLLERDEVICEADDED:
            return "SDL_CONTROLLERDEVICEADDED";
        case SDL_CONTROLLERDEVICEREMOVED:
            return "SDL_CONTROLLERDEVICEREMOVED";
        case SDL_CONTROLLERDEVICEREMAPPED:
            return "SDL_CONTROLLERDEVICEREMAPPED";
        case SDL_FINGERDOWN:
            return "SDL_FINGERDOWN";
        case SDL_FINGERUP:
            return "SDL_FINGERUP";
        case SDL_FINGERMOTION:
            return "SDL_FINGERMOTION";
        case SDL_USEREVENT:
            return "SDL_USEREVENT";
        default:
            if (type == USER_REMOTEBUTTONEVENT) {
                return "USER_REMOTEBUTTONEVENT";
            }
            return "OTHER";
    }
}

#endif

void app_quit_confirm() {
    static const char *btn_txts[] = {translatable("Cancel"), translatable("OK"), ""};
    lv_obj_t *mbox = lv_msgbox_create_i18n(NULL, NULL, locstr("Quit Moonlight?"), btn_txts, false);
    lv_obj_add_event_cb(mbox, quit_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

void app_request_exit() {
    global->running = false;
}

bool app_is_running() {
    return global->running;
}

bool app_is_decoder_valid(app_t *app) {
    return app->ss4s.selection.video_module != NULL && app->ss4s.selection.audio_module != NULL;
}

#if FEATURE_EMBEDDED_SHELL
bool app_has_embedded(app_t *app) {
    return version_info_valid(&app->embed_version);
}

bool app_decoder_or_embedded_present(app_t *app) {
    return app_is_decoder_valid(app) || app_has_embedded(app);
}
#endif

static void libs_init(app_t *app, int argc, char *argv[]) {
    SS4S_SetLoggingFunction(commons_ss4s_logf);
    int errno;
    if ((errno = SS4S_ModulesList(&app->ss4s.modules, &app->os_info)) != 0) {
        commons_log_error("SS4S", "Can't load modules list: %s", strerror(errno));
    }
    SS4S_ModulePreferences module_preferences = {
            .audio_module = app_configuration->audio_backend,
            .video_module = app_configuration->decoder,
    };
    SS4S_ModulesSelect(&app->ss4s.modules, &module_preferences, &app->ss4s.selection, true);
    commons_log_info("APP", "Video module: %s (requested %s)", SS4S_ModuleInfoGetName(app->ss4s.selection.video_module),
                     module_preferences.video_module);
    commons_log_info("APP", "Audio module: %s (requested %s)", SS4S_ModuleInfoGetName(app->ss4s.selection.audio_module),
                     module_preferences.audio_module);

#if FEATURE_EMBEDDED_SHELL
    if (!app_is_decoder_valid(app)) {
        // Check if moonlight-embedded is installed
        if (embed_check_version(&app->embed_version) == 0) {
            char *embed_version_str = version_info_str(&app->embed_version);
            commons_log_info("APP", "Moonlight Embedded version: %s", embed_version_str);
            free(embed_version_str);
        }
    }
#endif

    SS4S_Config ss4s_config = {
            .audioDriver = SS4S_ModuleInfoGetId(app->ss4s.selection.audio_module),
            .videoDriver = SS4S_ModuleInfoGetId(app->ss4s.selection.video_module),
    };
    SS4S_Init(argc, argv, &ss4s_config);

    SS4S_GetAudioCapabilitiesByCodecs(&app->ss4s.audio_cap, SS4S_AUDIO_PCM_S16LE | SS4S_AUDIO_OPUS);
    SS4S_GetVideoCapabilities(&app->ss4s.video_cap);


#if FEATURE_INPUT_LIBCEC
    cec_sdl_init(&app->cec, "Moonlight");
#endif
}

static void quit_confirm_cb(lv_event_t *e) {
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (lv_msgbox_get_active_btn(mbox) == 1) {
        app_request_exit();
    } else {
        lv_msgbox_close_async(mbox);
    }
}
