set(SERVER_UI_SOURCES server_window.cpp server_window.h client_permissions.cpp client_permissions.h
    server_settings.cpp server_settings.h ../client/transport.cpp ../client/transport.h
    ../client/attachment.cpp ../client/attachment.h
    ../client/connection_info.cpp ../client/connection_info.h)
qt_add_executable(wechat-server main.cpp ${SERVER_UI_SOURCES})
target_include_directories(wechat-server PRIVATE ../client)
target_link_libraries(wechat-server PRIVATE Qt6::Widgets Qt6::Network Qt6::WebSockets)
set_target_properties(wechat-server PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "微信轻聊服务端" MACOSX_BUNDLE_GUI_IDENTIFIER "local.wechatlite.server"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
if(MSVC)
    target_compile_options(wechat-server PRIVATE /utf-8 /W4)
endif()
qt_add_executable(server-gui-tests tests.cpp ${SERVER_UI_SOURCES})
target_include_directories(server-gui-tests PRIVATE ../client)
target_link_libraries(server-gui-tests PRIVATE Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Test)
if(MSVC)
    target_compile_options(server-gui-tests PRIVATE /utf-8 /W4)
endif()
if(WIN32)
    set(RELAY_EXECUTABLE "${PROJECT_SOURCE_DIR}/target/release/wechat-relay.exe")
    foreach(target wechat-server server-gui-tests)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/target/release/wechat-relay.exe" "$<TARGET_FILE_DIR:${target}>/wechat-relay.exe")
    endforeach()
else()
    set(RELAY_EXECUTABLE "${PROJECT_SOURCE_DIR}/target/release/wechat-relay")
    foreach(target wechat-server server-gui-tests)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/target/release/wechat-relay" "$<TARGET_FILE_DIR:${target}>/wechat-relay")
    endforeach()
endif()
add_custom_target(server-gui-runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RELAY_EXECUTABLE}" "$<TARGET_FILE_DIR:wechat-server>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RELAY_EXECUTABLE}" "$<TARGET_FILE_DIR:server-gui-tests>"
    DEPENDS wechat-server server-gui-tests)
add_test(NAME server-gui-tests COMMAND server-gui-tests -platform offscreen -o server-gui-tests.txt,txt)
install(TARGETS wechat-server BUNDLE DESTINATION . RUNTIME DESTINATION .)
