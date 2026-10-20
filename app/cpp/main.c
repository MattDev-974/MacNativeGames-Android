/* main.c - Android entry point (NativeActivity, no Java in the APK).
 *
 * Mirrors games/totk/ios/Sources/main.m: the platform shell only owns the
 * window and the input queue, then hands control to the host runtime.  On
 * Android the window is an ANativeWindow, input arrives through AInputEvent
 * instead of GameController.framework, and AAudio replaces AVFAudio
 * (docs/02-portage-android.md).
 */
#include <android/log.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <stdio.h>
#include <libgen.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mng_boot.h"
#include "nvn.h"
#include "vk.h"

#define TAG "mnge-main"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef struct {
    struct android_app *app;
    ANativeWindow      *window;
    int                 animating;
    int                 running;
    int                 guest_ready;
    int                 booted;
    uint64_t            frames;
} Engine;

static Engine engine;

/* Fires when the dynamic loader maps libtotk.so, before ANativeActivity_onCreate.
 * If this line never reaches logcat, the library itself failed to load. */
__attribute__((constructor))
static void on_library_load(void)
{
    LOGI("libtotk.so loaded (pid %d, uid %d)", (int)getpid(), (int)getuid());
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void handle_cmd(struct android_app *app, int32_t cmd)
{
    LOGI("cmd %d", (int)cmd);

    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        engine.window = app->window;
        if (!engine.window) { LOGE("INIT_WINDOW with no window"); return; }
        LOGI("window %dx%d", ANativeWindow_getWidth(engine.window),
             ANativeWindow_getHeight(engine.window));
        ANativeActivity_setWindowFlags(app->activity,
                                       AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
        /* The surface is usually still 1x1 here: the swapchain is created from
         * the main loop once Android has laid the window out. */
        engine.animating = 1;
        LOGI("window attached (%dx%d), deferring Vulkan to the first real size",
             ANativeWindow_getWidth(engine.window), ANativeWindow_getHeight(engine.window));
        break;

    case APP_CMD_TERM_WINDOW:
        engine.animating = 0;
        vk_suspend();
        engine.window = NULL;
        break;

    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        vk_invalidate_swapchain();
        break;

    case APP_CMD_GAINED_FOCUS:
        engine.animating = 1;
        break;

    case APP_CMD_LOST_FOCUS:
        engine.animating = 0;
        break;

    case APP_CMD_DESTROY:
        nvn_shutdown();
        vk_teardown();
        engine.running = 0;
        break;
    }
}

/* Gamepad -> Switch Pro Controller.  The Mac side does this in
 * runtime/gamepad.m + controller_map.h; Android delivers the same axes and
 * buttons through AInputEvent.  TODO: feed runtime/input.c. */
static int32_t handle_input(struct android_app *app, AInputEvent *event)
{
    (void)app;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY &&
        AKeyEvent_getKeyCode(event) == AKEYCODE_BACK) {
        ANativeActivity_finish(app->activity);
        return 1;
    }
    return 0;
}

void android_main(struct android_app *app)
{
    LOGI("android_main entered");

    engine.app = app;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    app->userData = &engine;

    nvn_init();
    engine.running = 1;
    engine.animating = 0;

    LOGI("MacNativeGames / Android starting");

    while (engine.running) {
        int events = 0;
        void *data = NULL;

        /* Block while nothing is animating; spin at the display rate while it is. */
        int ident = ALooper_pollOnce(engine.animating ? 0 : -1, NULL, &events, &data);
        if (ident == ALOOPER_POLL_ERROR) { LOGE("looper error"); break; }
        if (data) {
            struct android_poll_source *source = data;
            source->process(app, source);
        }
        if (app->destroyRequested) { engine.running = 0; break; }

        /* Bring the graphics stack up as soon as the surface has a real size.
         * resume() builds the device on the first call and only rebuilds the
         * surface + swapchain after a background/foreground cycle. */
        if (engine.window && !vk_ready()) {
            if (ANativeWindow_getWidth(engine.window) > 1 &&
                ANativeWindow_getHeight(engine.window) > 1) {
                if (vk_resume(engine.window) != 0) {
                    LOGE("Vulkan bring-up failed, see mnge-vk above");
                } else {
                    if (!engine.guest_ready) {
                        nvn_guest_init();
                        engine.guest_ready = 1;
                    }
                    nvn_rebind_pool();
                    LOGI("rendering %dx%d", vk_width(), vk_height());

                    /* The rewritten modules live in nativeLibraryDir, the only
                     * executable, read-only location Android offers.  Our own
                     * library sits there too, so dladdr gives the directory. */
                    if (!engine.booted) {
                        engine.booted = 1;
                        Dl_info info;
                        char dir[4096] = ".";
                        if (dladdr((void *)(uintptr_t)mng_boot_run, &info) && info.dli_fname) {
                            snprintf(dir, sizeof(dir), "%s", info.dli_fname);
                            char *slash = strrchr(dir, '/');
                            if (slash) *slash = 0;
                        }
                        LOGI("starting the game runtime from %s", dir);
                        if (mng_boot_run(dir) != 0)
                            LOGE("game runtime did not start; see mnge-boot above");
                    }
                }
            } else {
                usleep(20000);   /* still 1x1, give the compositor a moment */
            }
        }

        if (engine.animating && vk_ready()) {
            nvn_guest_frame(now_ns());
            engine.frames++;
            if (engine.frames % 120 == 1)
                LOGI("frame %llu", (unsigned long long)engine.frames);
        }
    }

    nvn_shutdown();
    vk_teardown();
    LOGI("stopped after %llu frames", (unsigned long long)engine.frames);

    /* native_app_glue destroys the app and returns from the thread as soon as
     * android_main returns, which finishes the activity.  The activity must
     * only end when Android asks for it, so park here instead of returning. */
    for (;;) {
        int events = 0;
        void *data = NULL;
        ALooper_pollOnce(-1, NULL, &events, &data);
        if (data) {
            struct android_poll_source *source = data;
            source->process(app, source);
        }
    }
}
