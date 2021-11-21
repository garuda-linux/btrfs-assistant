cmake_minimum_required(VERSION 3.5)

project(btrfs-assistant VERSION 0.1 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set_directory_properties(PROPERTIES CLEAN_NO_CUSTOM 1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt5 COMPONENTS Widgets LinguistTools REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets LinguistTools REQUIRED)

file(GLOB TS_FILES ${PROJECT_SOURCE_DIR}/translations/*.ts)

configure_file(config.h.in config.h @ONLY)

set(PROJECT_SOURCES
        main.cpp
        btrfs-assistant.cpp
        btrfs-assistant.h
        btrfs-assistant.ui
        icons.qrc
        ${CMAKE_CURRENT_BINARY_DIR}/config.h
)

qt5_create_translation(FILES_TS ${PROJECT_SOURCES} ${TS_FILES})

add_executable(btrfs-assistant
    ${PROJECT_SOURCES} ${FILES_TS}
)

install(FILES ${FILES_TS} DESTINATION ${CMAKE_INSTALL_PREFIX}/share/btrfs-assistant/translations/)
install(FILES btrfs-assistant.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications/)
install(FILES btrfs-assistant.png DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps/)
install(TARGETS btrfs-assistant RUNTIME DESTINATION bin)


target_link_libraries(btrfs-assistant PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
