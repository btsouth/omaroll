cmake_minimum_required(VERSION 3.24)

project(Omaroll VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Multimedia Qml Quick QuickControls2 Test)
qt_standard_project_setup(REQUIRES 6.8)

add_library(omaroll_core STATIC
  src/app/AppSettings.cpp
  src/app/AppSettings.h
  src/app/DemoLibrary.cpp
  src/app/DemoLibrary.h
  src/app/SingleInstance.cpp
  src/app/SingleInstance.h
  src/app/OpenRequest.cpp
  src/app/OpenRequest.h
  src/library/CaptureRecord.h
  src/library/CaptureRoles.h
  src/library/CaptureModel.cpp
  src/library/CaptureModel.h
  src/library/CaptureFilterModel.cpp
  src/library/CaptureFilterModel.h
  src/library/DuplicateIndex.cpp
  src/library/DuplicateIndex.h
  src/library/MediaInspector.cpp
  src/library/MediaInspector.h
  src/library/SimilarityIndex.cpp
  src/library/SimilarityIndex.h
  src/library/MediaDateIndex.cpp
  src/library/MediaDateIndex.h
  src/sources/CaptureScanner.cpp
  src/sources/CaptureScanner.h
  src/sources/CaptureLocations.cpp
  src/sources/CaptureLocations.h
  src/actions/ActionLauncher.cpp
  src/actions/ActionLauncher.h
  src/actions/ActionRegistry.cpp
  src/actions/ActionRegistry.h
  src/actions/TailscalePeers.cpp
  src/actions/TailscalePeers.h
  src/matte/HueExtractor.cpp
  src/matte/HueExtractor.h
  src/matte/MatteComposer.cpp
  src/matte/MatteComposer.h
  src/matte/MatteProvider.cpp
  src/matte/MatteProvider.h
  src/pdf/PdfInspector.cpp
  src/pdf/PdfInspector.h
  src/pdf/PdfProvider.cpp
  src/pdf/PdfProvider.h
  src/pdf/PdfSupport.cpp
  src/pdf/PdfSupport.h
  src/search/OcrIndex.cpp
  src/search/OcrIndex.h
  src/search/QrDetector.cpp
  src/search/QrDetector.h
  src/thumbs/ThumbnailCache.cpp
  src/thumbs/ThumbnailCache.h
  src/thumbs/ThumbnailProvider.cpp
  src/thumbs/ThumbnailProvider.h
  src/theme/OmarchyTheme.cpp
  src/theme/OmarchyTheme.h
)

target_include_directories(omaroll_core PUBLIC src)
target_link_libraries(omaroll_core PUBLIC Qt6::Concurrent Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
target_compile_definitions(omaroll_core PUBLIC OMAROLL_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(omaroll_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

qt_add_resources(omaroll_core "omaroll_demo_media"
  PREFIX "/omaroll/demo"
  BASE "resources/demo"
  FILES
    resources/demo/alpine-dawn.jpg
    resources/demo/black-sand-coast.jpg
    resources/demo/blue-hour-street.jpg
    resources/demo/botanical-still-life.jpg
    resources/demo/canyon-river.jpg
    resources/demo/coastal-wave.jpg
    resources/demo/clouds-over-canyon.mp4
    resources/demo/desert-curves.jpg
    resources/demo/dew-ferns.jpg
    resources/demo/forest-falls.jpg
    resources/demo/green-road.jpg
    resources/demo/ocean-surface.mp4
    resources/demo/rain-courtyard.jpg
    resources/demo/rain-reading-room.jpg
    resources/demo/rain-window.mp4
    resources/demo/storm-meadow.jpg
    resources/demo/sunlit-stair.jpg
    resources/demo/winter-cabin.jpg
)

qt_add_executable(omaroll src/app/main.cpp)

qt_add_resources(omaroll "omaroll_icons"
  PREFIX "/icons"
  FILES
    resources/icons/omaroll.svg
)

set(OMAROLL_QML_FILES
  qml/Main.qml
  qml/components/CaptureCard.qml
  qml/components/CaptureGrid.qml
  qml/components/DayHeader.qml
  qml/components/Chrome.qml
  qml/components/FilterBar.qml
  qml/components/LibraryBrowserSheet.qml
  qml/components/PillButton.qml
  qml/components/ConfirmSheet.qml
  qml/components/DetailSheet.qml
  qml/components/MatteSheet.qml
  qml/components/SettingsSheet.qml
  qml/components/AlbumNameSheet.qml
  qml/components/ExportSheet.qml
  qml/components/RenameSheet.qml
  qml/components/TailscaleSheet.qml
  qml/components/TextReviewSheet.qml
)

qt_add_qml_module(omaroll
  URI Omaroll
  VERSION 1.0
  QML_FILES ${OMAROLL_QML_FILES}
)

target_link_libraries(omaroll PRIVATE omaroll_core Qt6::Multimedia Qt6::QuickControls2)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(omaroll PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS omaroll RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.tsouth89.omaroll.desktop
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES packaging/io.github.tsouth89.omaroll.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
install(FILES resources/icons/omaroll.svg
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
  RENAME io.github.tsouth89.omaroll.svg)
# The Hyprland opacity opt-out, for users to require until it lands upstream.
install(FILES packaging/hypr/omaroll.lua
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/omaroll/hypr)
install(FILES LICENSE
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/omaroll)
install(FILES README.md PRIVACY.md SUPPORT.md CHANGELOG.md
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/omaroll)

# Tests
include(CTest)
if(BUILD_TESTING)
  add_executable(omaroll_benchmark_library EXCLUDE_FROM_ALL tests/benchmark_library.cpp)
  target_link_libraries(omaroll_benchmark_library PRIVATE omaroll_core)
  qt_add_executable(omaroll_tests tests/tst_omaroll.cpp)
  target_link_libraries(omaroll_tests PRIVATE omaroll_core Qt6::Test)
  add_test(NAME omaroll_tests COMMAND omaroll_tests)
  add_test(NAME omaroll_version COMMAND omaroll --version)
  set_tests_properties(omaroll_version PROPERTIES
    PASS_REGULAR_EXPRESSION "omaroll ${PROJECT_VERSION}")
  # The suite touches QImage and QGuiApplication paths, so it needs a platform
  # plugin; offscreen keeps it runnable in a headless checkout or CI.
  set(omaroll_test_environment
    "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software;QT_IM_MODULE=compose;PULSE_SERVER=unix:/nonexistent")
  set_tests_properties(omaroll_tests PROPERTIES ENVIRONMENT
    "${omaroll_test_environment};LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.supp")

  # The UI suite loads the real Main.qml and drives it with synthetic input.
  # The module's files are compiled into the omaroll executable, so a second
  # process cannot import them from there; a qmldir over symlinks to the
  # sources gives the test the same module, always current.
  set(uitest_module ${CMAKE_BINARY_DIR}/uitest-imports/Omaroll)
  file(MAKE_DIRECTORY ${uitest_module})
  set(uitest_qmldir "module Omaroll\n")
  foreach(qml_file IN LISTS OMAROLL_QML_FILES)
    get_filename_component(qml_name ${qml_file} NAME_WE)
    file(CREATE_LINK ${CMAKE_SOURCE_DIR}/${qml_file} ${uitest_module}/${qml_name}.qml SYMBOLIC)
    string(APPEND uitest_qmldir "${qml_name} 1.0 ${qml_name}.qml\n")
  endforeach()
  file(WRITE ${uitest_module}/qmldir "${uitest_qmldir}")

  qt_add_executable(omaroll_ui_tests tests/tst_ui.cpp)
  target_link_libraries(omaroll_ui_tests PRIVATE omaroll_core Qt6::Multimedia Qt6::QuickControls2 Qt6::Test)
  target_compile_definitions(omaroll_ui_tests PRIVATE
    OMAROLL_QML_IMPORT_PATH="${CMAKE_BINARY_DIR}/uitest-imports")
  add_test(NAME omaroll_ui_tests COMMAND omaroll_ui_tests)
  # Qt Multimedia initializes the system FFmpeg, VAAPI/CUDA and D-Bus stacks,
  # whose process-lifetime allocations make LeakSanitizer noisy. The core suite
  # retains leak detection; this real-window suite still runs ASan and UBSan.
  set_tests_properties(omaroll_ui_tests PROPERTIES ENVIRONMENT
    "${omaroll_test_environment};ASAN_OPTIONS=detect_leaks=0")
endif()
