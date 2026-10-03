# Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
#
# backend-common: shared native-API implementations that the CEF and
# webview backends both link against. Each backend pre-parses the
# backend-specific laufey_value_t into the plain structs declared in
# laufey_backend_common.h, then calls into the functions defined here.
#
# Parallels backend-winit-common/, which serves the same role for the
# winit-based backends (winit + servo).

cmake_minimum_required(VERSION 3.19)

# On macOS the parent project may set CMAKE_OSX_DEPLOYMENT_TARGET via CEF's
# cmake config. Honor that target so the static lib matches the executable
# linker target and we don't get "built for newer macOS" warnings.
if(APPLE AND DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(_inherited_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

if(APPLE)
  project(laufey_backend_common VERSION 1.0.0 LANGUAGES C CXX OBJCXX)
else()
  project(laufey_backend_common VERSION 1.0.0 LANGUAGES C CXX)
endif()

if(APPLE AND DEFINED _inherited_target)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${_inherited_target}" CACHE STRING
      "Minimum OS X deployment version" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LAUFEY_COMMON_SRCS
  src/parse_options.cc
  src/keymap_vk.cc
  src/title_badge.cc
  src/laufey_value.cc
  src/test_hooks.cc
  src/scheme_registry.cc
  src/data_dir.cc
  src/launch_config.cc
  src/launch_args.cc
  src/single_instance.cc
  src/passkey.cc
  src/window_state.cc
  src/io.cc
  src/shortcuts.cc
  src/login_item.cc
  src/menu_common.cc
  src/notifications.cc
  src/auth_session.cc
)

if(WIN32)
  list(APPEND LAUFEY_COMMON_SRCS src/single_instance_win.cc)
else()
  list(APPEND LAUFEY_COMMON_SRCS src/single_instance_posix.cc)
endif()

if(APPLE)
  list(APPEND LAUFEY_COMMON_SRCS
    src/notifications_mac.mm
    src/dialog_mac.mm
    src/clipboard_mac.mm
    src/keymap_mac.mm
    src/dock_mac.mm
    src/open_url_mac.mm
    src/menu_mac.mm
    src/tray_mac.mm
    src/single_instance_mac.mm
    src/passkey_mac.mm
    src/window_mac.mm
    src/io_mac.mm
    src/shortcuts_mac.mm
    src/login_item_mac.mm
    src/auth_session_mac.mm
  )
elseif(UNIX)
  list(APPEND LAUFEY_COMMON_SRCS
    src/notifications_linux.cc
    src/dialog_linux.cc
    src/clipboard_linux.cc
    src/keymap_gdk.cc
    src/menu_linux.cc
    src/tray_linux.cc
    src/io_linux.cc
    src/shortcuts_linux.cc
    src/login_item_linux.cc
    src/scheme_body_stream_linux.cc
  )
elseif(WIN32)
  list(APPEND LAUFEY_COMMON_SRCS
    src/strings_win.cc
    src/notifications_win.cc
    src/dialog_win.cc
    src/clipboard_win.cc
    src/tray_win.cc
    src/passkey_win.cc
    src/window_win.cc
    src/io_win.cc
    src/shortcuts_win.cc
    src/login_item_win.cc
  )
endif()

add_library(laufey_backend_common STATIC ${LAUFEY_COMMON_SRCS})

target_include_directories(laufey_backend_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../capi/include
)

if(WIN32)
  # WIC PNG decode + Shell_NotifyIcon
  target_link_libraries(laufey_backend_common PUBLIC
    windowscodecs
    ole32
    shell32
    gdi32
    user32
    advapi32
    dwmapi
    shcore
    uuid
    # Toast notifications (WinRT activation) and the version resource read
    # for the app's display name.
    runtimeobject
    version
  )
elseif(APPLE)
  find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  # Passkeys (ASAuthorizationController); present since macOS 10.15, the
  # passkey providers are checked for 12+ at runtime.
  find_library(AUTHENTICATIONSERVICES_FRAMEWORK AuthenticationServices REQUIRED)
  # Global shortcuts (RegisterEventHotKey) and launch at login (SMAppService,
  # macOS 13+, checked at runtime).
  find_library(CARBON_FRAMEWORK Carbon REQUIRED)
  find_library(SERVICEMANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
  target_link_libraries(laufey_backend_common PUBLIC
    ${USERNOTIFICATIONS_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    ${COCOA_FRAMEWORK}
    ${AUTHENTICATIONSERVICES_FRAMEWORK}
    ${CARBON_FRAMEWORK}
    ${SERVICEMANAGEMENT_FRAMEWORK}
  )
  target_compile_options(laufey_backend_common PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>
  )
elseif(UNIX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LAUFEY_COMMON_GTK REQUIRED gtk+-3.0)
  # Global shortcuts on X11: key grabs over a private XCB connection (libxcb
  # is already a dependency of GTK's X11 backend).
  pkg_check_modules(LAUFEY_COMMON_XCB REQUIRED xcb)
  target_include_directories(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_INCLUDE_DIRS}
    ${LAUFEY_COMMON_XCB_INCLUDE_DIRS}
  )
  # No appindicator build dependency: tray_linux.cc dlopen()s the
  # Ayatana or legacy appindicator library at runtime. CMAKE_DL_LIBS
  # covers dlopen/dlsym on toolchains that still need -ldl.
  target_link_libraries(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_LIBRARIES}
    ${LAUFEY_COMMON_XCB_LIBRARIES}
    ${CMAKE_DL_LIBS}
  )
  target_compile_options(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_CFLAGS_OTHER}
  )
endif()

# The single-instance server runs on its own thread.
find_package(Threads REQUIRED)
target_link_libraries(laufey_backend_common PUBLIC Threads::Threads)

set_target_properties(laufey_backend_common PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

# Unit tests for the platform-independent pieces (plain asserts, no test
# framework). The parent project calls enable_testing() so these show up in
# `ctest --test-dir <build>`; -DLAUFEY_BUILD_TESTS=OFF skips them.
option(LAUFEY_BUILD_TESTS "Build backend-common unit tests" ON)
if(LAUFEY_BUILD_TESTS)
  add_executable(laufey_scheme_registry_test tests/scheme_registry_test.cc)
  target_link_libraries(laufey_scheme_registry_test PRIVATE
    laufey_backend_common
  )
  target_link_libraries(laufey_scheme_registry_test PRIVATE Threads::Threads)
  add_test(NAME laufey_scheme_registry_test
           COMMAND laufey_scheme_registry_test)

  # Writes (and removes) a laufey-launch.json next to the test binary.
  add_executable(laufey_launch_config_test tests/launch_config_test.cc)
  target_link_libraries(laufey_launch_config_test PRIVATE
    laufey_backend_common
  )
  add_test(NAME laufey_launch_config_test
           COMMAND laufey_launch_config_test)

  # The host's own command line: laufey's options, the "--" terminator and
  # the deep-link switch filter.
  add_executable(laufey_launch_args_test tests/launch_args_test.cc)
  target_link_libraries(laufey_launch_args_test PRIVATE
    laufey_backend_common
  )
  add_test(NAME laufey_launch_args_test
           COMMAND laufey_launch_args_test)

  # Takes (and releases) real single-instance locks under per-run app ids.
  add_executable(laufey_single_instance_test tests/single_instance_test.cc)
  target_link_libraries(laufey_single_instance_test PRIVATE
    laufey_backend_common
  )
  add_test(NAME laufey_single_instance_test
           COMMAND laufey_single_instance_test)

  # Passkey options parser, envelopes and ceremony lifecycle (no OS API).
  add_executable(laufey_passkey_test tests/passkey_test.cc)
  target_link_libraries(laufey_passkey_test PRIVATE
    laufey_backend_common
    Threads::Threads
  )
  add_test(NAME laufey_passkey_test COMMAND laufey_passkey_test)

  # The rendezvous behind the synchronous UI-thread calls (laufey_sync_call.h)
  # under load; the `tsan` CI job also runs it under ThreadSanitizer.
  add_executable(laufey_sync_call_test tests/sync_call_test.cc)
  target_link_libraries(laufey_sync_call_test PRIVATE
    laufey_backend_common
    Threads::Threads
  )
  add_test(NAME laufey_sync_call_test COMMAND laufey_sync_call_test)
  set_tests_properties(laufey_sync_call_test PROPERTIES TIMEOUT 300)

  # Window-state bookkeeping: state diffing, size constraints, normal-bounds
  # settling, screen matching and the app-lifetime flags (no OS API).
  add_executable(laufey_window_state_test tests/window_state_test.cc)
  target_link_libraries(laufey_window_state_test PRIVATE
    laufey_backend_common
  )
  add_test(NAME laufey_window_state_test COMMAND laufey_window_state_test)

  # Drag-and-drop dispatch, drag-out path checks, the file dialog slot and
  # option copying, file URIs, CF_HTML and clipboard format lists (no OS API).
  add_executable(laufey_io_test tests/io_test.cc)
  target_link_libraries(laufey_io_test PRIVATE laufey_backend_common)
  add_test(NAME laufey_io_test COMMAND laufey_io_test)

  # The native check on URLs the page asks the reserved open-external call
  # to open (laufey_external_links.h): string fixtures only.
  add_executable(laufey_external_links_test tests/external_links_test.cc)
  target_link_libraries(laufey_external_links_test PRIVATE laufey_backend_common)
  add_test(NAME laufey_external_links_test COMMAND laufey_external_links_test)

  # Accelerator parsing, the shortcut registry (with a fake OS side) and the
  # autostart entry format (no OS API).
  add_executable(laufey_system_test tests/system_test.cc)
  target_link_libraries(laufey_system_test PRIVATE laufey_backend_common)
  add_test(NAME laufey_system_test COMMAND laufey_system_test)

  # Menu template parsing and accelerators, the context-menu session, and
  # the notification core over a fake platform (no OS API).
  add_executable(laufey_menu_notifications_test
                 tests/menu_notifications_test.cc)
  target_link_libraries(laufey_menu_notifications_test PRIVATE
    laufey_backend_common
    Threads::Threads
  )
  add_test(NAME laufey_menu_notifications_test
           COMMAND laufey_menu_notifications_test)

  # UI-thread task dispatch (the delivery guarantee, over a fake queue) and
  # the auth session core: argument validation, the one-session slot and
  # the exactly-once result (no OS API).
  add_executable(laufey_auth_main_thread_test
                 tests/auth_main_thread_test.cc)
  target_link_libraries(laufey_auth_main_thread_test PRIVATE
    laufey_backend_common
    Threads::Threads
  )
  add_test(NAME laufey_auth_main_thread_test
           COMMAND laufey_auth_main_thread_test)

  if(UNIX AND NOT APPLE)
    # The freedesktop notification client against a mock server on a
    # private D-Bus session bus (GTestDBus; skipped without dbus-daemon).
    add_executable(laufey_notifications_dbus_test
                   tests/notifications_dbus_test.cc)
    target_link_libraries(laufey_notifications_dbus_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_notifications_dbus_test
             COMMAND laufey_notifications_dbus_test)

    # The WebKitGTK custom-scheme response body: writes never block, a
    # pollable reader on a GMainContext, the queued-bytes cap, a reader that
    # went away (no WebKit).
    add_executable(laufey_scheme_body_stream_test
                   tests/scheme_body_stream_test.cc)
    target_link_libraries(laufey_scheme_body_stream_test PRIVATE
      laufey_backend_common
      Threads::Threads
    )
    add_test(NAME laufey_scheme_body_stream_test
             COMMAND laufey_scheme_body_stream_test)
    set_tests_properties(laufey_notifications_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77)
    # The stand-in notification server the Linux native e2e runs
    # (scripts/native-e2e-run.sh --menus-notifications).
    add_executable(laufey_mock_notification_server
                   tests/mock_notification_server.cc)
    target_link_libraries(laufey_mock_notification_server PRIVATE
      ${LAUFEY_COMMON_GTK_LIBRARIES}
    )
    target_include_directories(laufey_mock_notification_server PRIVATE
      ${LAUFEY_COMMON_GTK_INCLUDE_DIRS}
    )

    # The XDND drag source the Linux native e2e drags real files out of
    # with xdotool (scripts/native-e2e-run.sh --io).
    add_executable(laufey_xdnd_source tests/xdnd_source.cc)
    target_link_libraries(laufey_xdnd_source PRIVATE
      ${LAUFEY_COMMON_GTK_LIBRARIES}
    )
    target_include_directories(laufey_xdnd_source PRIVATE
      ${LAUFEY_COMMON_GTK_INCLUDE_DIRS}
    )

    # The GlobalShortcuts portal client against a mock portal on a private
    # D-Bus session bus (GTestDBus; skipped when dbus-daemon is missing).
    add_executable(laufey_shortcuts_portal_test
                   tests/shortcuts_portal_test.cc)
    target_link_libraries(laufey_shortcuts_portal_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_shortcuts_portal_test
             COMMAND laufey_shortcuts_portal_test)
    set_tests_properties(laufey_shortcuts_portal_test PROPERTIES
                         SKIP_RETURN_CODE 77)
  endif()

  if(WIN32)
    # The OLE drag source the Windows native e2e drags real files out of
    # with SendInput (scripts/native-e2e-run.sh --io).
    add_executable(laufey_ole_drag_source tests/ole_drag_source.cc)
    target_link_libraries(laufey_ole_drag_source PRIVATE ole32 shell32 user32)
    if(MSVC)
      target_link_options(laufey_ole_drag_source PRIVATE
        /ENTRY:wmainCRTStartup)
    endif()
  endif()

  if(APPLE)
    # ASAuthorization error mapping (synthetic NSErrors, no ceremony).
    add_executable(laufey_passkey_mac_test tests/passkey_mac_test.mm)
    target_link_libraries(laufey_passkey_mac_test PRIVATE
      laufey_backend_common
    )
    target_compile_options(laufey_passkey_mac_test PRIVATE -fobjc-arc)
    add_test(NAME laufey_passkey_mac_test COMMAND laufey_passkey_mac_test)

    # The drag pasteboard's files (the CEF backend's external drops).
    add_executable(laufey_io_mac_test tests/io_mac_test.mm)
    target_link_libraries(laufey_io_mac_test PRIVATE laufey_backend_common)
    target_compile_options(laufey_io_mac_test PRIVATE -fobjc-arc)
    add_test(NAME laufey_io_mac_test COMMAND laufey_io_mac_test)
  endif()
endif()
