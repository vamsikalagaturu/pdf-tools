# libpdf-tools.so for the Android APK; configured by build.sh with the NDK toolchain file.
cmake_minimum_required(VERSION 3.20)
project(pdf_tools_android C CXX)

set(PT_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MUPDF_THIRD_LIBRARY "" CACHE FILEPATH "libmupdf-third.a")

# clang does not know the desktop build's -Wno-clobbered
add_compile_options(-Wno-unknown-warning-option)

# ptcore and ptcli come from the desktop build; the GUI target there needs desktop raylib, so it is off.
set(PT_BUILD_GUI OFF CACHE BOOL "" FORCE)
set(PT_USE_PCRE2 OFF CACHE BOOL "" FORCE)
set(PT_USE_FONTCONFIG OFF CACHE BOOL "" FORCE)
add_subdirectory(${PT_NATIVE} native EXCLUDE_FROM_ALL)
target_link_libraries(ptcore PUBLIC ${MUPDF_THIRD_LIBRARY})

set(PLATFORM Android CACHE STRING "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
add_subdirectory(${PT_NATIVE}/thirdparty/raylib raylib EXCLUDE_FROM_ALL)

# The window's embedded files, generated as in native/CMakeLists.txt (pt_embed is defined there).
set(PT_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PT_PY_ASSETS ${PT_NATIVE}/../src/pdf_tools/assets)
pt_embed(${PT_GEN}/ui_font.c pt_ui_font ${PT_NATIVE}/assets/NotoSans-Regular.ttf)
pt_embed(${PT_GEN}/ui_font_bold.c pt_ui_font_bold ${PT_NATIVE}/assets/NotoSans-Bold.ttf)
pt_embed(${PT_GEN}/man.c pt_asset_man ${PT_NATIVE}/../man/pdf-tools.1)
file(GLOB PT_ICONS ${PT_PY_ASSETS}/icons/*.svg)
set(PT_ICON_SOURCES)
set(PT_ICON_TABLE "")
set(PT_ICON_DECLS "")
foreach(icon ${PT_ICONS})
  get_filename_component(name ${icon} NAME_WE)
  string(MAKE_C_IDENTIFIER ${name} id)
  pt_embed(${PT_GEN}/icon_${id}.c pt_icon_${id} ${icon})
  list(APPEND PT_ICON_SOURCES ${PT_GEN}/icon_${id}.c)
  string(APPEND PT_ICON_DECLS "extern const unsigned char pt_icon_${id}[];\nextern const int pt_icon_${id}_size;\n")
  string(APPEND PT_ICON_TABLE "  {\"${name}\", pt_icon_${id}, &pt_icon_${id}_size},\n")
endforeach()
file(WRITE ${PT_GEN}/icons.c.tmp "#include \"ui_icons.h\"\n${PT_ICON_DECLS}const pt_icon_data pt_icons[] = {\n${PT_ICON_TABLE}  {0, 0, 0}\n};\n")
configure_file(${PT_GEN}/icons.c.tmp ${PT_GEN}/icons.c COPYONLY)

# All shared window sources, unmodified; new ones are picked up on reconfigure.
file(GLOB PT_UI_SOURCES CONFIGURE_DEPENDS ${PT_NATIVE}/src/ui/*.c)
add_library(pdf-tools SHARED
  ${PT_UI_SOURCES} glfw_stubs.c android_main.c
  ${PT_GEN}/ui_font.c ${PT_GEN}/ui_font_bold.c ${PT_GEN}/man.c ${PT_GEN}/icons.c ${PT_ICON_SOURCES})
target_include_directories(pdf-tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PT_NATIVE}/src/ui ${PT_NATIVE}/thirdparty/raygui/src
  ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_options(pdf-tools PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Android replacements for a few calls, swapped in per file so the shared sources stay untouched (android_glue.h).
set_source_files_properties(${PT_NATIVE}/src/ui/app.c PROPERTIES COMPILE_DEFINITIONS
  "main=pt_app_main;InitWindow=pt_android_init_window;file_dialogs_poll=pt_android_frame;run_detached=pt_android_run_detached")
set_source_files_properties(${PT_NATIVE}/src/ui/widgets.c PROPERTIES COMPILE_DEFINITIONS
  "GetCharPressed=pt_android_char_pressed;GetMouseWheelMoveV=pt_android_wheel")
# C++ link for MuPDF's HarfBuzz (libc++ is static: ANDROID_STL defaults to c++_static)
set_target_properties(pdf-tools PROPERTIES LINKER_LANGUAGE CXX)
# -u keeps the glue's entry point, which nothing in the library references
target_link_options(pdf-tools PRIVATE -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,--gc-sections)
target_link_libraries(pdf-tools PRIVATE ptcli raylib android log EGL GLESv2 m)
