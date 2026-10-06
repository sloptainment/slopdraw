cmake_minimum_required(VERSION 3.21)
project(mydraw LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui)

# Headless core: QtGui only, never QtWidgets (#35).
add_library(mydraw_core src/document.cpp src/spiro.cpp src/brush.cpp src/powerstroke.cpp src/ora.cpp src/references.cpp vendor/miniz.c)
target_include_directories(mydraw_core PUBLIC src vendor)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYPAINT REQUIRED IMPORTED_TARGET libmypaint)
target_link_libraries(mydraw_core PUBLIC Qt6::Gui PkgConfig::MYPAINT)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(mydraw src/main.cpp)
target_link_libraries(mydraw PRIVATE mydraw_core Qt6::Widgets)
# Power of 10 rule 10 (#622): src/ builds warning-free at the pedantic setting; tests/ and vendor/ C are not held to it (#644).
target_compile_options(mydraw_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
target_compile_options(mydraw PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(mydraw PRIVATE BRUSH_DIR="${CMAKE_SOURCE_DIR}/brushes")

enable_testing()
add_executable(test_document tests/test_document.cpp)
target_link_libraries(test_document PRIVATE mydraw_core)
target_compile_definitions(test_document PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden" TEST_PENCIL="${CMAKE_SOURCE_DIR}/tests/pencil.myb" BRUSH_DIR="${CMAKE_SOURCE_DIR}/brushes")
add_test(NAME document COMMAND test_document)
