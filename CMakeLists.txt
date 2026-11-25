cmake_minimum_required(VERSION 3.19)
project(Libreworks VERSION 0.3.37 LANGUAGES C CXX)

include(cmake/LwBrand.cmake)
configure_file(${CMAKE_SOURCE_DIR}/src/brand/Brand.h.in
               ${CMAKE_BINARY_DIR}/generated/Brand.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test Concurrent Svg)
find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp libwebpdemux libwebpmux)
find_package(GIF REQUIRED)
pkg_check_modules(TIFF REQUIRED IMPORTED_TARGET libtiff-4)
pkg_check_modules(AVIF IMPORTED_TARGET libavif)
pkg_check_modules(JXL IMPORTED_TARGET libjxl libjxl_threads)
# v0.2.5 formats
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
find_package(JPEG REQUIRED)
pkg_check_modules(HEIF IMPORTED_TARGET libheif)
pkg_check_modules(LIBRAW IMPORTED_TARGET libraw_r)
pkg_check_modules(OPENEXR IMPORTED_TARGET OpenEXR)
pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
pkg_check_modules(IMAGEQUANT IMPORTED_TARGET imagequant)
find_path(ZOPFLIPNG_INCLUDE_DIR zopfli/zopflipng_lib.h)
find_library(ZOPFLIPNG_LIBRARY zopflipng)
option(LW_STRICT_WARNINGS "Extra warnings for LibreWorks' own code (-Wshadow, -Wuseless-cast, -Wnon-virtual-dtor, ...)" OFF)
option(LW_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (debug/CI)" OFF)
if(LW_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
# warning flags for first-party targets (vendored code is built with -w)
set(LW_WARNING_FLAGS -Wall -Wextra)
if(LW_STRICT_WARNINGS)
  list(APPEND LW_WARNING_FLAGS -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-qual -Wduplicated-branches -Wrange-loop-construct)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND LW_WARNING_FLAGS -Wuseless-cast -Wduplicated-cond -Wlogical-op)
  endif()
endif()
option(LW_BUILD_JPEG_ENCODERS "Build the private MozJPEG and jpegli encoder libraries from third_party/archives" ON)

include(GNUInstallDirs)
# Vendored pixel-art scalers: libxbr-standalone (hqx + xBR, LGPL-2.1+) and xBRZ 1.8 (GPL-3)
add_library(lwpixelvendor STATIC
  third_party/libxbr/xbr.c third_party/libxbr/hq2x.c third_party/libxbr/hq3x.c third_party/libxbr/hq4x.c
  third_party/xbrz/xbrz.cpp)
target_include_directories(lwpixelvendor PUBLIC third_party)
set_target_properties(lwpixelvendor PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(lwpixelvendor PRIVATE -w)

add_library(lwcore STATIC
  src/core/PixelScale.cpp src/core/PixelScale.h
  src/core/Object.cpp src/core/Object.h
  src/core/Paths.cpp src/core/Paths.h
  src/core/FindReplace.cpp src/core/FindReplace.h
  src/core/Tablet.cpp src/core/Tablet.h
  src/core/Fills.cpp src/core/Fills.h src/core/CorePrefs.h
  src/core/Stroke.cpp src/core/Stroke.h
  src/core/Effects.cpp src/core/Effects.h
  src/core/Render.cpp src/core/Render.h
  src/core/Document.cpp src/core/Document.h
  src/core/Filters.cpp src/core/Filters.h
  src/core/SelectionMask.cpp src/core/SelectionMask.h
  src/core/Retouch.cpp src/core/Retouch.h
  src/core/BrushEngine.cpp src/core/BrushEngine.h
  src/core/Styles.cpp src/core/Styles.h
  src/core/Symbols.cpp
  src/core/Groups.cpp
  src/core/Procedural.cpp src/core/Procedural.h
  src/core/Transform.cpp src/core/Transform.h
  src/core/PixelEdit.cpp src/core/PixelEdit.h
  src/core/AutoShape.cpp src/core/AutoShape.h
  src/core/FxExtra.cpp src/core/FxExtra.h
  src/core/Trace.cpp src/core/Trace.h
  src/io/ImageIO.cpp src/io/ImageIO.h src/io/Codecs.h
  src/io/Quantize.cpp src/io/Quantize.h
  src/io/ExportPresets.cpp src/io/ExportPresets.h
  src/io/Batch.cpp src/io/Batch.h
  src/io/GifIO.cpp src/io/WebpIO.cpp src/io/ApngIO.cpp
  src/io/AvifIO.cpp src/io/JxlIO.cpp src/io/NetpbmIO.cpp
  src/io/TiffIO.cpp src/io/TgaIO.cpp
  src/io/BmpIO.cpp src/io/IconIO.cpp src/io/PcxIO.cpp src/io/HdrIO.cpp src/io/TextureIO.cpp
  src/io/HeifIO.cpp src/io/RawIO.cpp src/io/PngIO.cpp src/io/QoiIO.cpp src/io/CHeaderIO.cpp src/io/JpegIO.cpp
  src/io/PsdIO.cpp src/io/OraIO.cpp src/io/XcfIO.cpp
  src/io/SvgIO.cpp src/io/DocIO.cpp src/io/DocIO.h
  src/io/Zip.cpp src/io/Zip.h
  src/io/LwdFormat.cpp src/io/LwdFormat.h
  src/io/HtmlExport.cpp src/io/HtmlExport.h
)
target_include_directories(lwcore PUBLIC src)
target_include_directories(lwcore PRIVATE third_party ${JPEG_INCLUDE_DIRS})
target_link_libraries(lwcore PRIVATE lwpixelvendor)
target_link_libraries(lwcore PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Svg ZLIB::ZLIB PkgConfig::WEBP GIF::GIF PkgConfig::TIFF
  PkgConfig::PNG ${JPEG_LIBRARIES} ${CMAKE_DL_LIBS})
if(HEIF_FOUND)
  target_link_libraries(lwcore PUBLIC PkgConfig::HEIF)
  target_compile_definitions(lwcore PUBLIC LW_HAVE_HEIF=1)
endif()
if(LIBRAW_FOUND)
  target_link_libraries(lwcore PUBLIC PkgConfig::LIBRAW)
  target_compile_definitions(lwcore PUBLIC LW_HAVE_RAW=1)
endif()
if(OPENEXR_FOUND)
  target_link_libraries(lwcore PUBLIC PkgConfig::OPENEXR)
  target_compile_definitions(lwcore PUBLIC LW_HAVE_OPENEXR=1)
endif()
if(ZSTD_FOUND)
  target_link_libraries(lwcore PUBLIC PkgConfig::ZSTD)
  target_compile_definitions(lwcore PUBLIC LW_HAVE_ZSTD=1)
endif()
if(IMAGEQUANT_FOUND)
  target_link_libraries(lwcore PUBLIC PkgConfig::IMAGEQUANT)
  target_compile_definitions(lwcore PUBLIC LW_HAVE_IMAGEQUANT=1)
endif()
if(ZOPFLIPNG_INCLUDE_DIR AND ZOPFLIPNG_LIBRARY)
  target_include_directories(lwcore PRIVATE ${ZOPFLIPNG_INCLUDE_DIR})
  target_link_libraries(lwcore PUBLIC ${ZOPFLIPNG_LIBRARY})
  target_compile_definitions(lwcore PUBLIC LW_HAVE_ZOPFLI=1)
endif()

# Private MozJPEG / jpegli builds: libjpeg.so.62-ABI libraries renamed liblw-mozjpeg.so / liblw-jpegli.so,
# loaded with dlopen(RTLD_LOCAL | RTLD_DEEPBIND) so they never clash with the system libjpeg.
if(LW_BUILD_JPEG_ENCODERS)
  include(ExternalProject)
  find_program(PATCHELF patchelf REQUIRED)
  set(LW_JPEG_ENC_DIR ${CMAKE_BINARY_DIR}/jpeg-encoders)
  ExternalProject_Add(mozjpeg_ext
    URL ${CMAKE_SOURCE_DIR}/third_party/archives/mozjpeg-4.1.5.tar.gz
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_SHARED=ON -DENABLE_STATIC=OFF
               -DPNG_SUPPORTED=OFF -DWITH_TURBOJPEG=OFF -DWITH_JAVA=OFF -DWITH_SIMD=OFF -DREQUIRE_SIMD=OFF
               -DWITH_ARITH_ENC=OFF -DWITH_ARITH_DEC=OFF -DWITH_12BIT=OFF -DWITH_MEM_SRCDST=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target jpeg
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${LW_JPEG_ENC_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/libjpeg.so.62.3.0 ${LW_JPEG_ENC_DIR}/liblw-mozjpeg.so
      COMMAND ${PATCHELF} --set-soname liblw-mozjpeg.so ${LW_JPEG_ENC_DIR}/liblw-mozjpeg.so
    BUILD_BYPRODUCTS ${LW_JPEG_ENC_DIR}/liblw-mozjpeg.so
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  ExternalProject_Add(jpegli_ext
    URL ${CMAKE_SOURCE_DIR}/third_party/archives/jpegli-src.tar.xz
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF
               -DJPEGLI_ENABLE_TOOLS=OFF -DJPEGLI_ENABLE_BENCHMARK=OFF -DJPEGLI_ENABLE_DEVTOOLS=OFF
               -DJPEGLI_ENABLE_DOXYGEN=OFF -DJPEGLI_ENABLE_MANPAGES=OFF -DJPEGLI_ENABLE_JNI=OFF
               -DJPEGLI_ENABLE_SJPEG=OFF -DJPEGLI_ENABLE_OPENEXR=OFF -DJPEGLI_ENABLE_SKCMS=OFF
               -DJPEGLI_FORCE_SYSTEM_HWY=ON -DJPEGLI_FORCE_SYSTEM_LCMS2=ON -DJPEGLI_BUNDLE_LIBPNG=NO
               -DJPEGLI_ENABLE_JPEGLI_LIBJPEG=ON -DJPEGLI_LIBJPEG_LIBRARY_SOVERSION=62 -DJPEGLI_LIBJPEG_LIBRARY_VERSION=62.3.0
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target jpeg -j
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${LW_JPEG_ENC_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/lib/jpegli/libjpeg.so.62.3.0 ${LW_JPEG_ENC_DIR}/liblw-jpegli.so
      COMMAND ${PATCHELF} --set-soname liblw-jpegli.so ${LW_JPEG_ENC_DIR}/liblw-jpegli.so
    BUILD_BYPRODUCTS ${LW_JPEG_ENC_DIR}/liblw-jpegli.so
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  add_dependencies(lwcore mozjpeg_ext jpegli_ext)
  target_compile_definitions(lwcore PUBLIC LW_JPEG_ENCODER_BUILD_DIR="${LW_JPEG_ENC_DIR}")
  install(FILES ${LW_JPEG_ENC_DIR}/liblw-mozjpeg.so ${LW_JPEG_ENC_DIR}/liblw-jpegli.so DESTINATION lib/${LW_LIB_DIR_NAME})
endif()
if(AVIF_FOUND)
  target_link_libraries(lwcore PUBLIC PkgConfig::AVIF)
  target_compile_definitions(lwcore PUBLIC LW_HAVE_AVIF=1)
endif()
if(JXL_FOUND)
  target_link_libraries(lwcore PUBLIC PkgConfig::JXL)
  target_compile_definitions(lwcore PUBLIC LW_HAVE_JXL=1)
endif()
target_compile_definitions(lwcore PUBLIC LW_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lwcore PRIVATE ${LW_WARNING_FLAGS})
endif()

add_library(lwui STATIC
  src/ui/AppState.cpp src/ui/AppState.h
  src/ui/Theme.cpp src/ui/Theme.h
  src/ui/WidgetEnable.cpp src/ui/WidgetEnable.h
  src/ui/ComboFit.cpp src/ui/ComboFit.h
  src/ui/Icons.cpp src/ui/Icons.h
  src/ui/AppIcons.cpp src/ui/AppIcons.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/icons.qrc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/brand.qrc
  src/ui/Canvas.cpp src/ui/Canvas.h
  src/ui/Rulers.cpp src/ui/Rulers.h src/ui/ViewGeometry.h
  src/ui/DocumentView.cpp src/ui/DocumentView.h
  src/ui/ToolsPanel.cpp src/ui/ToolsPanel.h
  src/ui/PopupSlider.cpp src/ui/PopupSlider.h
  src/ui/ColorPicker.cpp src/ui/ColorPicker.h
  src/ui/ZoomField.cpp src/ui/ZoomField.h
  src/ui/PropertyInspector.cpp src/ui/PropertyInspector.h
  src/ui/WideFontCombo.cpp src/ui/WideFontCombo.h
  src/ui/FileDialogs.cpp src/ui/FileDialogs.h
  src/ui/ProceduralDialog.cpp src/ui/ProceduralDialog.h
  src/ui/TraceDialog.cpp src/ui/TraceDialog.h
  src/ui/PixelUpscaleDialog.cpp src/ui/PixelUpscaleDialog.h
  src/ui/FindPanel.cpp src/ui/FindPanel.h
  src/ui/Panels.cpp src/ui/Panels.h
  src/ui/DragReorder.cpp src/ui/DragReorder.h
  src/ui/Dialogs.cpp src/ui/Dialogs.h
  src/ui/EditStrokeDialog.cpp src/ui/EditStrokeDialog.h
  src/ui/ClipboardImage.cpp src/ui/ClipboardImage.h
  src/ui/GradientEditor.cpp src/ui/GradientEditor.h
  src/ui/LiquifyDialog.cpp src/ui/LiquifyDialog.h
  src/ui/BatchDialog.cpp src/ui/BatchDialog.h
  src/ui/PreferencesDialog.cpp src/ui/PreferencesDialog.h
  src/ui/Shortcuts.cpp src/ui/Shortcuts.h
  src/ui/KeyboardPrefs.cpp src/ui/KeyboardPrefs.h
  src/ui/TabletPrefs.cpp src/ui/TabletPrefs.h
  src/ui/FaviconDialog.cpp src/ui/FaviconDialog.h
  src/ui/MainWindow.cpp src/ui/MainWindow.h
)
target_link_libraries(lwui PUBLIC lwcore Qt6::Widgets)
# v0.3.4: the eyedropper uses xdg-desktop-portal's PickColor on Wayland when Qt's D-Bus module is there
find_package(Qt6 QUIET COMPONENTS DBus)
if(TARGET Qt6::DBus)
  target_link_libraries(lwui PUBLIC Qt6::DBus)
  target_compile_definitions(lwui PUBLIC LW_HAVE_DBUS=1)
endif()
add_executable(${LW_EXE_NAME} src/main.cpp)
target_link_libraries(${LW_EXE_NAME} PRIVATE lwui)
set_target_properties(${LW_EXE_NAME} PROPERTIES OUTPUT_NAME ${LW_EXE_NAME} RUNTIME_OUTPUT_NAME ${LW_EXE_NAME})
# Stable alias so scripts can depend on "libreworks" even for a Bonbon build.
if(NOT TARGET libreworks)
  add_executable(libreworks ALIAS ${LW_EXE_NAME})
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lwui PRIVATE ${LW_WARNING_FLAGS})
  target_compile_options(${LW_EXE_NAME} PRIVATE ${LW_WARNING_FLAGS})
endif()

enable_testing()
add_executable(roundtrip_test tests/roundtrip_test.cpp tests/formats_v025.cpp tests/cheader_test.cpp)
target_link_libraries(roundtrip_test PRIVATE lwcore)
target_compile_definitions(roundtrip_test PRIVATE LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME roundtrip COMMAND roundtrip_test)
set_tests_properties(roundtrip PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(features_test tests/features_test.cpp)
target_link_libraries(features_test PRIVATE lwcore Qt6::Test)
add_test(NAME features COMMAND features_test)
set_tests_properties(features PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(effects_test tests/effects_test.cpp)
target_link_libraries(effects_test PRIVATE lwcore Qt6::Test)
add_test(NAME effects COMMAND effects_test)
add_executable(procedural_test tests/procedural_test.cpp)
target_link_libraries(procedural_test PRIVATE lwcore Qt6::Test)
add_test(NAME procedural COMMAND procedural_test)
set_tests_properties(procedural PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(effects PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(ui_smoke_test tests/ui_smoke_test.cpp)
target_link_libraries(ui_smoke_test PRIVATE lwui Qt6::Test)
target_compile_definitions(ui_smoke_test PRIVATE LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ui_smoke COMMAND ui_smoke_test)
set_tests_properties(ui_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# 0.3.4: the default window layout (1400x900, and 1366x768) needs a screen that big
add_test(NAME ui_smoke_layout COMMAND ui_smoke_test propertyInspectorDefaultHeightFitsContent)
set_tests_properties(ui_smoke_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen:configfile=${CMAKE_SOURCE_DIR}/tests/data/screen-1920x1080.json")

add_executable(v030_test tests/v030_test.cpp)
target_link_libraries(v030_test PRIVATE lwui Qt6::Test)
target_compile_definitions(v030_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>")
add_dependencies(v030_test ${LW_EXE_NAME})
add_test(NAME v030 COMMAND v030_test)
set_tests_properties(v030 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v031_test tests/v031_test.cpp)
target_link_libraries(v031_test PRIVATE lwui Qt6::Test)
target_compile_definitions(v031_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(v031_test ${LW_EXE_NAME})
add_test(NAME v031 COMMAND v031_test)
set_tests_properties(v031 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v032_test tests/v032_test.cpp)
target_link_libraries(v032_test PRIVATE lwui Qt6::Test)
target_compile_definitions(v032_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(v032_test ${LW_EXE_NAME})
add_test(NAME v032 COMMAND v032_test)
set_tests_properties(v032 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v033_test tests/v033_test.cpp)
target_link_libraries(v033_test PRIVATE lwui Qt6::Test)
add_dependencies(v033_test ${LW_EXE_NAME})
add_test(NAME v033 COMMAND v033_test)
set_tests_properties(v033 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v034_test tests/v034_test.cpp)
target_link_libraries(v034_test PRIVATE lwui Qt6::Test)
add_dependencies(v034_test ${LW_EXE_NAME})
add_test(NAME v034 COMMAND v034_test)
add_executable(v035_test tests/v035_test.cpp)
target_link_libraries(v035_test PRIVATE lwui Qt6::Test)
add_dependencies(v035_test ${LW_EXE_NAME})
add_test(NAME v035 COMMAND v035_test)
set_tests_properties(v035 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v036_test tests/v036_test.cpp)
target_link_libraries(v036_test PRIVATE lwui Qt6::Test)
add_dependencies(v036_test ${LW_EXE_NAME})
add_test(NAME v036 COMMAND v036_test)
set_tests_properties(v036 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v037_test tests/v037_test.cpp)
target_link_libraries(v037_test PRIVATE lwui Qt6::Test)
add_dependencies(v037_test ${LW_EXE_NAME})
add_test(NAME v037 COMMAND v037_test)
add_executable(v038_test tests/v038_test.cpp)
target_link_libraries(v038_test PRIVATE lwui Qt6::Test)
add_dependencies(v038_test ${LW_EXE_NAME})
add_test(NAME v038 COMMAND v038_test)
set_tests_properties(v038 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v039_test tests/v039_test.cpp)
target_link_libraries(v039_test PRIVATE lwui Qt6::Test)
add_dependencies(v039_test ${LW_EXE_NAME})
add_test(NAME v039 COMMAND v039_test)
set_tests_properties(v039 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v040_test tests/v040_test.cpp)
target_link_libraries(v040_test PRIVATE lwui Qt6::Test)
add_dependencies(v040_test ${LW_EXE_NAME})
target_compile_definitions(v040_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v040 COMMAND v040_test)

add_executable(v041_test tests/v041_test.cpp)
target_link_libraries(v041_test PRIVATE lwui Qt6::Test)
add_dependencies(v041_test ${LW_EXE_NAME})
add_test(NAME v041 COMMAND v041_test)
set_tests_properties(v041 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v042_test tests/v042_test.cpp)
target_link_libraries(v042_test PRIVATE lwui Qt6::Test)
add_dependencies(v042_test ${LW_EXE_NAME})
add_test(NAME v042 COMMAND v042_test)
set_tests_properties(v042 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v043_test tests/v043_test.cpp)
target_link_libraries(v043_test PRIVATE lwui Qt6::Test)
add_dependencies(v043_test ${LW_EXE_NAME})
target_compile_definitions(v043_test PRIVATE LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v043 COMMAND v043_test)
set_tests_properties(v043 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v044_test tests/v044_test.cpp)
target_link_libraries(v044_test PRIVATE lwui Qt6::Test)
add_dependencies(v044_test ${LW_EXE_NAME})
add_test(NAME v044 COMMAND v044_test)
set_tests_properties(v044 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v045_test tests/v045_test.cpp)
target_link_libraries(v045_test PRIVATE lwui Qt6::Test)
add_dependencies(v045_test ${LW_EXE_NAME})
add_test(NAME v045 COMMAND v045_test)
set_tests_properties(v045 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(v046_test tests/v046_test.cpp)
target_link_libraries(v046_test PRIVATE lwui Qt6::Test)
add_dependencies(v046_test ${LW_EXE_NAME})
add_test(NAME v046 COMMAND v046_test)
set_tests_properties(v046 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(v047_test tests/v047_test.cpp)
target_link_libraries(v047_test PRIVATE lwui Qt6::Test)
add_dependencies(v047_test ${LW_EXE_NAME})
add_test(NAME v047 COMMAND v047_test)
set_tests_properties(v047 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(v048_test tests/v048_test.cpp)
target_link_libraries(v048_test PRIVATE lwui Qt6::Test)
add_dependencies(v048_test ${LW_EXE_NAME})
target_compile_definitions(v048_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v048 COMMAND v048_test)
set_tests_properties(v048 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v049_test tests/v049_test.cpp)
target_link_libraries(v049_test PRIVATE lwui Qt6::Test)
add_dependencies(v049_test ${LW_EXE_NAME})
target_compile_definitions(v049_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v049 COMMAND v049_test)
set_tests_properties(v049 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(v050_test tests/v050_test.cpp)
target_link_libraries(v050_test PRIVATE lwui Qt6::Test)
add_dependencies(v050_test ${LW_EXE_NAME})
target_compile_definitions(v050_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v050 COMMAND v050_test)
set_tests_properties(v050 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v051_test tests/v051_test.cpp)
target_link_libraries(v051_test PRIVATE lwui Qt6::Test)
add_dependencies(v051_test ${LW_EXE_NAME})
target_compile_definitions(v051_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v051 COMMAND v051_test)
set_tests_properties(v051 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v052_test tests/v052_test.cpp)
target_link_libraries(v052_test PRIVATE lwui Qt6::Test)
add_dependencies(v052_test ${LW_EXE_NAME})
target_compile_definitions(v052_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v052 COMMAND v052_test)
set_tests_properties(v052 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v053_test tests/v053_test.cpp)
target_link_libraries(v053_test PRIVATE lwui Qt6::Test)
add_dependencies(v053_test ${LW_EXE_NAME})
target_compile_definitions(v053_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v053 COMMAND v053_test)
set_tests_properties(v053 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v054_test tests/v054_test.cpp)
target_link_libraries(v054_test PRIVATE lwui Qt6::Test)
add_dependencies(v054_test ${LW_EXE_NAME})
target_compile_definitions(v054_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v054 COMMAND v054_test)
set_tests_properties(v054 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v055_test tests/v055_test.cpp)
target_link_libraries(v055_test PRIVATE lwui Qt6::Test)
add_dependencies(v055_test ${LW_EXE_NAME})
target_compile_definitions(v055_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v055 COMMAND v055_test)
set_tests_properties(v055 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v056_test tests/v056_test.cpp)
target_link_libraries(v056_test PRIVATE lwui Qt6::Test)
add_dependencies(v056_test ${LW_EXE_NAME})
target_compile_definitions(v056_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v056 COMMAND v056_test)
set_tests_properties(v056 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v057_test tests/v057_test.cpp)
target_link_libraries(v057_test PRIVATE lwui Qt6::Test)
add_dependencies(v057_test ${LW_EXE_NAME})
target_compile_definitions(v057_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v057 COMMAND v057_test)
set_tests_properties(v057 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v058_test tests/v058_test.cpp)
target_link_libraries(v058_test PRIVATE lwui Qt6::Test)
add_dependencies(v058_test ${LW_EXE_NAME})
target_compile_definitions(v058_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v058 COMMAND v058_test)
set_tests_properties(v058 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v059_test tests/v059_test.cpp)
target_link_libraries(v059_test PRIVATE lwui Qt6::Test)
add_dependencies(v059_test ${LW_EXE_NAME})
target_compile_definitions(v059_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v059 COMMAND v059_test)
set_tests_properties(v059 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v060_test tests/v060_test.cpp)
target_link_libraries(v060_test PRIVATE lwui Qt6::Test)
add_dependencies(v060_test ${LW_EXE_NAME})
target_compile_definitions(v060_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v060 COMMAND v060_test)
set_tests_properties(v060 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v061_test tests/v061_test.cpp)
target_link_libraries(v061_test PRIVATE lwui Qt6::Test)
add_dependencies(v061_test ${LW_EXE_NAME})
target_compile_definitions(v061_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v061 COMMAND v061_test)
set_tests_properties(v061 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v062_test tests/v062_test.cpp)
target_link_libraries(v062_test PRIVATE lwui Qt6::Test)
add_dependencies(v062_test ${LW_EXE_NAME})
target_compile_definitions(v062_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v062 COMMAND v062_test)
set_tests_properties(v062 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v063_test tests/v063_test.cpp)
target_link_libraries(v063_test PRIVATE lwui Qt6::Test)
add_dependencies(v063_test ${LW_EXE_NAME})
target_compile_definitions(v063_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v063 COMMAND v063_test)
set_tests_properties(v063 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v064_test tests/v064_test.cpp)
target_link_libraries(v064_test PRIVATE lwui Qt6::Test)
add_dependencies(v064_test ${LW_EXE_NAME})
target_compile_definitions(v064_test PRIVATE LW_EXE="$<TARGET_FILE:${LW_EXE_NAME}>" LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v064 COMMAND v064_test)
set_tests_properties(v064 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v065_test tests/v065_test.cpp)
target_link_libraries(v065_test PRIVATE lwui Qt6::Test)
add_dependencies(v065_test ${LW_EXE_NAME})
add_test(NAME v065 COMMAND v065_test)
set_tests_properties(v065 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=900000" TIMEOUT 900)
add_executable(v066_test tests/v066_test.cpp)
target_link_libraries(v066_test PRIVATE lwui Qt6::Test)
target_compile_definitions(v066_test PRIVATE LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(v066_test ${LW_EXE_NAME})
add_test(NAME v066 COMMAND v066_test)
set_tests_properties(v066 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(v067_test tests/v067_test.cpp)
target_link_libraries(v067_test PRIVATE lwui Qt6::Test)
add_dependencies(v067_test ${LW_EXE_NAME})
add_test(NAME v067 COMMAND v067_test)
set_tests_properties(v067 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=600000" TIMEOUT 600)
add_executable(v068_test tests/v068_test.cpp)
target_link_libraries(v068_test PRIVATE lwui Qt6::Test)
add_dependencies(v068_test ${LW_EXE_NAME})
add_test(NAME v068 COMMAND v068_test)
set_tests_properties(v068 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=900000" TIMEOUT 900)
add_executable(v069_test tests/v069_test.cpp)
target_link_libraries(v069_test PRIVATE lwui Qt6::Test)
add_dependencies(v069_test ${LW_EXE_NAME})
set_tests_properties(v040 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(v037 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(v034 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# the eyedropper on two monitors, one high-DPI (Qt's offscreen platform with two screens)
add_test(NAME picker_screens COMMAND v034_test pickerAcrossScreens pickerSamplesWhatIsShown)
set_tests_properties(picker_screens PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen:configfile=${CMAKE_SOURCE_DIR}/tests/data/offscreen-two-screens.json")
# a real X11 screen grab, at 1x and on a high-DPI (2x and 1.5x) screen, when Xvfb is installed
find_program(XVFB_RUN xvfb-run)
if(XVFB_RUN)
  foreach(scale 1 2 1.5)
    add_test(NAME picker_x11_${scale} COMMAND ${XVFB_RUN} -a -s "-screen 0 1600x1200x24"
             ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=xcb QT_SCALE_FACTOR=${scale} QT_ENABLE_HIGHDPI_SCALING=1
             $<TARGET_FILE:v034_test> pickerSamplesWhatIsShown pickerCancels pickerWorksFromModalDialogs)
  endforeach()
  add_test(NAME v069 COMMAND ${XVFB_RUN} -a -s "-screen 0 1600x1200x24"
           ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=xcb QTEST_FUNCTION_TIMEOUT=300000
           $<TARGET_FILE:v069_test>)
  set_tests_properties(v069 PROPERTIES TIMEOUT 300)
endif()

add_executable(v070_test tests/v070_test.cpp)
target_link_libraries(v070_test PRIVATE lwui Qt6::Test)
add_dependencies(v070_test ${LW_EXE_NAME})
add_test(NAME v070 COMMAND v070_test)
set_tests_properties(v070 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(v071_test tests/v071_test.cpp)
target_link_libraries(v071_test PRIVATE lwui Qt6::Test)
add_dependencies(v071_test ${LW_EXE_NAME})
add_test(NAME v071 COMMAND v071_test)
set_tests_properties(v071 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(v072_test tests/v072_test.cpp)
target_link_libraries(v072_test PRIVATE lwui Qt6::Test)
add_dependencies(v072_test ${LW_EXE_NAME})
target_compile_definitions(v072_test PRIVATE LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v072 COMMAND v072_test)
set_tests_properties(v072 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(v073_test tests/v073_test.cpp)
target_link_libraries(v073_test PRIVATE lwui Qt6::Test)
add_dependencies(v073_test ${LW_EXE_NAME})
target_compile_definitions(v073_test PRIVATE LW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME v073 COMMAND v073_test)
set_tests_properties(v073 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# AddressSanitizer: keep leak detection on, suppress the known fontconfig leak in ui_smoke.
if(LW_SANITIZE)
  set(_lw_asan "ASAN_OPTIONS=detect_leaks=1")
  set(_lw_ubsan "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
  set(_lw_lsan "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")
  get_property(_lw_tests DIRECTORY PROPERTY TESTS)
  foreach(_t ${_lw_tests})
    get_property(_env TEST ${_t} PROPERTY ENVIRONMENT)
    list(APPEND _env "${_lw_asan}" "${_lw_ubsan}" "${_lw_lsan}")
    set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${_env}")
  endforeach()
endif()

# Generated desktop / mime from brand templates
set(LW_DESKTOP_FILE ${CMAKE_BINARY_DIR}/packaging/${LW_DESKTOP_ID}.desktop)
set(LW_MIME_FILE ${CMAKE_BINARY_DIR}/packaging/${LW_PACKAGE_NAME}-lwd.xml)
configure_file(${CMAKE_SOURCE_DIR}/packaging/templates/app.desktop.in ${LW_DESKTOP_FILE} @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/packaging/templates/lwd.xml.in ${LW_MIME_FILE} @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/packaging/templates/copyright.in ${CMAKE_BINARY_DIR}/packaging/copyright @ONLY)

install(TARGETS ${LW_EXE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${LW_DESKTOP_FILE} DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${LW_MIME_FILE} DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
# App icons (hicolor). Libreworks keeps the historic 256-only install; Bonbon installs every size.
if(LW_IS_BONBON)
  foreach(_sz 16 22 24 32 48 64 128 256 512)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/icons/bonbon/${_sz}x${_sz}/bonbon.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps
            RENAME ${LW_ICON_NAME}.png)
  endforeach()
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/icons/bonbon/scalable/io.github.librebon.BonBon.svg
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
          RENAME ${LW_ICON_NAME}.svg)
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/io.github.librebon.BonBon.metainfo.xml
          DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  foreach(_sz 16 22 24 32 48 64 128 256)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/icons/bonbon/mimetypes/${_sz}x${_sz}/application-x-bonbon.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/mimetypes
            RENAME application-x-bonbon.png)
  endforeach()
  # Bonbon-only .lwd must still show the Libreworks document icon, not application-x-bonbon.
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/libreworks.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/mimetypes
          RENAME libreworks.png)
else()
  install(FILES packaging/libreworks.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME libreworks.png)
endif()

# The .tar.gz is self-contained and built from the AppImage AppDir by packaging/build-appimage.sh.
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "${LW_PACKAGE_NAME}")
set(CPACK_PACKAGE_VENDOR "${LW_ORG_NAME}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LW_PRODUCT_NAME}: native Fireworks MX 2004-style image editor with modern formats")
set(CPACK_PACKAGE_CONTACT "${LW_PRODUCT_NAME_LOWER}@localhost")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${LW_PRODUCT_NAME} <${LW_PRODUCT_NAME_LOWER}@localhost>")
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "")
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Pin shlibdeps floors to Debian trixie base versions (see packaging/debian/shlibs.local)
# so a trixie-security rebuild on the build box cannot inflate Depends
# (notably libopenexr-3-1-30 >= 3.2.12 while trixie ships 3.1.13).
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/packaging/CPackProjectConfig.cmake")
# shlibdeps adds the linked libraries (libheif1, libraw23t64, libopenexr-3-1-30, libimagequant0,
# libzopfli1, libzstd1, libhwy1t64 via the private jpegli build, ...); the HEIC codecs are libheif
# plugins that are loaded at run time, so they are listed explicitly.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libheif-plugin-libde265, libheif-plugin-x265, shared-mime-info")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-image-formats-plugins, qt6-wayland, libheif-plugin-dav1d")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_SOURCE_DIR}/packaging/debian/prerm")
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES TRUE)
include(CPack)

# Screenshot helper (not built by default): a document with one of every Auto Shape.
add_executable(autoshape_demo EXCLUDE_FROM_ALL tools/demo-doc/autoshape_demo.cpp)
target_link_libraries(autoshape_demo PRIVATE lwcore)

# theme-switch benchmark (v0.3.2): build with `ninja themebench`, run offscreen
add_executable(themebench EXCLUDE_FROM_ALL tools/themebench/themebench.cpp)
target_link_libraries(themebench PRIVATE lwui)

install(FILES ${CMAKE_BINARY_DIR}/packaging/copyright DESTINATION share/doc/${LW_DOC_DIR} RENAME copyright)
