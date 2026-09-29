cmake_minimum_required(VERSION 3.21)
project(mydraw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui)

# Headless core: QtGui only, never QtWidgets (#35).
add_library(mydraw_core src/document.cpp src/spiro.cpp)
target_include_directories(mydraw_core PUBLIC src)
target_link_libraries(mydraw_core PUBLIC Qt6::Gui)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(mydraw src/main.cpp)
target_link_libraries(mydraw PRIVATE mydraw_core Qt6::Widgets)

enable_testing()
add_executable(test_document tests/test_document.cpp)
target_link_libraries(test_document PRIVATE mydraw_core)
target_compile_definitions(test_document PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
add_test(NAME document COMMAND test_document)
