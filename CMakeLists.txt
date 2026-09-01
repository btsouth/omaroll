cmake_minimum_required(VERSION 3.24)

project(Omaroll VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Qml Quick QuickControls2 Test)
qt_standard_project_setup(REQUIRES 6.8)

add_library(omaroll_core STATIC
  src/app/AppSettings.cpp
  src/app/AppSettings.h
  src/app/DemoLibrary.cpp
  src/app/DemoLibrary.h
  src/app/SingleInstance.cpp
  src/app/SingleInstance.h
  src/library/CaptureRecord.h
  src/library/CaptureRoles.h
  src/library/CaptureModel.cpp
  src/library/CaptureModel.h
  src/library/CaptureFilterModel.cpp
  src/library/CaptureFilterModel.h
  src/sources/CaptureScanner.cpp
  src/sources/CaptureScanner.h
  src/sources/CaptureLocations.cpp
  src/sources/CaptureLocations.h
  src/actions/ActionLauncher.cpp
  src/actions/ActionLauncher.h
  src/actions/ActionRegistry.cpp
  src/actions/ActionRegistry.h
  src/matte/HueExtractor.cpp
  src/matte/HueExtractor.h
  src/matte/MatteComposer.cpp
  src/matte/MatteComposer.h
  src/matte/MatteProvider.cpp
  src/matte/MatteProvider.h
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

qt_add_executable(omaroll src/app/main.cpp)

qt_add_qml_module(omaroll
  URI Omaroll
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/components/CaptureCard.qml
    qml/components/CaptureGrid.qml
    qml/components/DayHeader.qml
    qml/components/Chrome.qml
    qml/components/FilterBar.qml
    qml/components/PillButton.qml
    qml/components/ConfirmSheet.qml
    qml/components/DetailSheet.qml
    qml/components/MatteSheet.qml
    qml/components/SettingsSheet.qml
)

target_link_libraries(omaroll PRIVATE omaroll_core Qt6::QuickControls2)

include(GNUInstallDirs)
install(TARGETS omaroll RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/omaroll.desktop
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES packaging/omaroll.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
install(FILES resources/icons/omaroll.svg
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)

# Tests
include(CTest)
if(BUILD_TESTING)
  qt_add_executable(omaroll_tests tests/tst_omaroll.cpp)
  target_link_libraries(omaroll_tests PRIVATE omaroll_core Qt6::Test)
  add_test(NAME omaroll_tests COMMAND omaroll_tests)
  # The suite touches QImage and QGuiApplication paths, so it needs a platform
  # plugin; offscreen keeps it runnable in a headless checkout or CI.
  set_tests_properties(omaroll_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
