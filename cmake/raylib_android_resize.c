// [God's Hand] raylib 5.5 on Android keeps the window size it saw first, so a
// game started while the phone is still upright keeps drawing (and placing
// touches) for a portrait window after it turns to landscape. Follow the
// window whenever it changes: rotation, system bars, coming back to the app.
// CMakeLists.txt inserts this into raylib's rcore_android.c for Android builds.
static void GhFollowWindowSize(struct android_app *app)
{
    if ((app->window == NULL) || (platform.device == EGL_NO_DISPLAY) || (platform.surface == EGL_NO_SURFACE)) return;

    // Buffers the size of the window (0, 0), then read that size back
    EGLint displayFormat = 0;
    eglGetConfigAttrib(platform.device, platform.config, EGL_NATIVE_VISUAL_ID, &displayFormat);
    ANativeWindow_setBuffersGeometry(app->window, 0, 0, displayFormat);
    int width = ANativeWindow_getWidth(app->window);
    int height = ANativeWindow_getHeight(app->window);
    if ((width <= 0) || (height <= 0)) return;
    if ((width == CORE.Window.screen.width) && (height == CORE.Window.screen.height)) return;

    CORE.Window.display.width = width;
    CORE.Window.display.height = height;
    CORE.Window.screen.width = width;
    CORE.Window.screen.height = height;
    CORE.Window.renderOffset.x = 0;
    CORE.Window.renderOffset.y = 0;
    CORE.Window.screenScale = MatrixIdentity();
    CORE.Window.currentFbo.width = width;
    CORE.Window.currentFbo.height = height;
    SetupViewport(width, height);
    CORE.Window.resizedLastFrame = true;
    TRACELOG(LOG_INFO, "DISPLAY: Window resized to %i x %i", width, height);
}

