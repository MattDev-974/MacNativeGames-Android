# CMakeLists.txt - the native half of the Android port.
#
# Only the NDK is required: no Gradle plugin, no prebuilt dependency, mirroring
# the macOS build which only needs Xcode.  See tools/build_apk.py.
cmake_minimum_required(VERSION 3.22)
project(totk C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The app is a game runtime: optimise for real phones, keep the frame loop tight.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -O2")

add_library(totk SHARED
    main.c
    vk.c
    nvn.c
    mng_image.c
    mng_loader.c
    mng_gate.c
    mng_svc.c
    mng_boot.c
    mng_tables.c
    gate.S
)

target_compile_definitions(totk PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR=1
)

target_include_directories(totk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The per-game tables (runtime_tables.h) are generated from the player's own
# file by tools/import_totk.py and compiled into the app, never committed.
if(MNG_GAME_TABLES)
    target_include_directories(totk PRIVATE ${MNG_GAME_TABLES})
    target_compile_definitions(totk PRIVATE MNG_WITH_GAME=1)
    message(STATUS "compiling game tables from ${MNG_GAME_TABLES}")
endif()

# Hide everything except the JNI entry point.  -ffixed-x18 is mandatory on
# Android/aarch64: x18 is the platform register and must never be allocated by
# the compiler (the game's x18 uses are rewritten by the patcher, same reason).
target_compile_options(totk PRIVATE -fvisibility=hidden -ffixed-x18)

# native_app_glue ships with the NDK and is the only Java-free activity shell.
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
# ANativeActivity_onCreate must stay exported or the activity cannot start.
set_target_properties(native_app_glue PROPERTIES C_VISIBILITY_PRESET default)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Pull the glue's translation unit in: nothing in our code references
# ANativeActivity_onCreate, so without -u the linker drops the archive member
# and the activity would fail to start.
target_link_options(totk PRIVATE "-Wl,-u,ANativeActivity_onCreate")

target_link_libraries(totk
    native_app_glue
    android
    log
    vulkan
    m
    atomic
)

# 16 KB page alignment: Android 15+ devices ship 16 KB kernels, and a 4 KB
# aligned .so fails to load there.  This is REQUIRED, not an optimisation.
target_link_options(totk PRIVATE "-Wl,-z,max-page-size=16384")
