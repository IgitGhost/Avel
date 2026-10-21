# avel.exe: the Qt Widgets GUI. All user-facing text lives here and goes through tr().

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets LinguistTools)

qt_standard_project_setup(
    REQUIRES 6.12
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES ru hy)

qt_add_executable(avel_app WIN32
    change_set_progress_dialog.cpp
    change_set_progress_dialog.h
    change_store_access.cpp
    change_store_access.h
    details_view.cpp
    details_view.h
    icon_loader.cpp
    icon_loader.h
    inventory_loader.cpp
    inventory_loader.h
    language_manager.cpp
    language_manager.h
    leftover_dialog.cpp
    leftover_dialog.h
    leftover_model.cpp
    leftover_model.h
    leftover_text.cpp
    leftover_text.h
    main.cpp
    main_window.cpp
    main_window.h
    program_filter_model.cpp
    program_filter_model.h
    program_table_model.cpp
    program_table_model.h
    program_text.cpp
    program_text.h
    right_elide_delegate.h
    undo_center_dialog.cpp
    undo_center_dialog.h
    uninstall_dialog.cpp
    uninstall_dialog.h
    uninstall_progress_dialog.cpp
    uninstall_progress_dialog.h)

set_target_properties(avel_app PROPERTIES
    OUTPUT_NAME avel
    QT_WINDOWS_APP_PROJECT_IDENTIFIER "Avel.App"
    # Avel reads every user's registry hive and removes machine-wide software,
    # so the GUI always runs elevated.
    QT_WINDOWS_APP_PROJECT_EXECUTION_LEVEL requireAdministrator)

target_compile_definitions(avel_app PRIVATE
    QT_NO_KEYWORDS
    QT_DISABLE_DEPRECATED_UP_TO=0x060C00)

target_link_libraries(avel_app PRIVATE avel_core Qt6::Widgets)

# Builds i18n/avel_{en,ru,hy}.ts into .qm files embedded under :/i18n.
# Refresh the .ts files after changing UI strings with: cmake --build <dir> --target update_translations
qt_add_translations(avel_app
    TS_FILE_BASE avel
    TS_FILE_DIR i18n
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none)

# Part of the top-level `deploy` target: copy the Qt DLLs and plugins next to avel.exe.
# Translations are embedded as resources, so windeployqt's copies aren't needed. Avel never
# touches the network, so the TLS/network plugins (and Qt6Network) are left out; the SVG icon
# engine stays for high-DPI icons.
add_custom_target(avel_app_deploy
    COMMAND "$<TARGET_FILE:Qt6::windeployqt>"
            --no-translations --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler
            --no-compiler-runtime
            --no-network --skip-plugin-types tls,networkinformation,generic
            "$<TARGET_FILE:avel_app>"
    DEPENDS avel_app
    COMMENT "Deploying Qt next to avel.exe"
    VERBATIM)
add_dependencies(deploy avel_app_deploy)

# Qt's own dialogs (standard buttons, file dialogs) need Qt's catalog. Qt ships Russian but not
# Armenian; an Armenian catalog is planned for the polish phase.
set(qt_translations_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
if(EXISTS "${qt_translations_dir}/qtbase_ru.qm")
    qt_add_resources(avel_app qt_translations
        PREFIX /i18n
        BASE "${qt_translations_dir}"
        FILES "${qt_translations_dir}/qtbase_ru.qm")
else()
    message(WARNING "qtbase_ru.qm not found in ${qt_translations_dir}; Qt dialogs will stay in English.")
endif()
