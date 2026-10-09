cmake_minimum_required(VERSION 3.21)
project(slopdraw LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui)

# Headless core: QtGui only, never QtWidgets (#35).
add_library(slopdraw_core src/document.cpp src/document_paths.cpp src/document_undo.cpp src/document_paint.cpp src/document_float.cpp src/spiro.cpp src/brush.cpp src/powerstroke.cpp src/ora.cpp src/references.cpp vendor/miniz.c)
target_include_directories(slopdraw_core PUBLIC src vendor)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYPAINT REQUIRED IMPORTED_TARGET libmypaint)
target_link_libraries(slopdraw_core PUBLIC Qt6::Gui PkgConfig::MYPAINT)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(slopdraw src/main.cpp src/canvas.cpp src/canvas_draw.cpp src/canvas_paint.cpp src/canvas_nodes.cpp src/canvas_select.cpp src/canvas_refs.cpp src/colorpicker.cpp src/navigator.cpp src/layerlist.cpp src/docks.cpp src/layersdock.cpp src/tooloptions.cpp src/toolsdock.cpp src/filemenu.cpp src/newdialog.cpp src/menus.cpp)
target_link_libraries(slopdraw PRIVATE slopdraw_core Qt6::Widgets)
# Power of 10 rule 10 (#622): src/ builds warning-free at the pedantic setting; tests/ and vendor/ C are not held to it (#644).
target_compile_options(slopdraw_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
target_compile_options(slopdraw PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(slopdraw PRIVATE BRUSH_DIR="${CMAKE_SOURCE_DIR}/brushes")

enable_testing()
add_executable(test_document tests/test_document.cpp)
target_link_libraries(test_document PRIVATE slopdraw_core)
target_compile_definitions(test_document PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden" TEST_PENCIL="${CMAKE_SOURCE_DIR}/tests/pencil.myb" BRUSH_DIR="${CMAKE_SOURCE_DIR}/brushes")
add_test(NAME document COMMAND test_document)
add_executable(test_resize_dialog tests/test_resize_dialog.cpp src/newdialog.cpp)
target_link_libraries(test_resize_dialog PRIVATE slopdraw_core Qt6::Widgets)
add_test(NAME resize_dialog COMMAND test_resize_dialog)
