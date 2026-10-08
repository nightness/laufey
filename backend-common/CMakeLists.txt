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
  src/cef_sandbox.cc
  src/cef_network_quiet.cc
  src/single_instance.cc
  src/passkey.cc
  src/window_state.cc
  src/io.cc
  src/shortcuts.cc
  src/login_item.cc
  src/menu_common.cc
  src/notifications.cc
  src/notification_auth.cc
  src/auth_session.cc
  src/platform_features.cc
  src/title_bar.cc
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
    src/title_bar_mac.mm
    src/secret_store_mac.mm
  )
elseif(UNIX)
  list(APPEND LAUFEY_COMMON_SRCS
    src/notifications_linux.cc
    src/launcher_entry_linux.cc
    src/dialog_linux.cc
    src/clipboard_linux.cc
    src/clipboard_data_control_linux.cc
    src/clipboard_x11_linux.cc
    src/session_lock_linux.cc
    src/wayland/ext-data-control-v1-protocol.c
    src/keymap_gdk.cc
    src/menu_linux.cc
    src/tray_linux.cc
    src/io_linux.cc
    src/shortcuts_linux.cc
    src/login_item_linux.cc
    src/scheme_body_stream_linux.cc
    src/platform_features_linux.cc
    src/cookie_db_linux.cc
    src/title_bar_linux.cc
    src/secret_store_linux.cc
    src/termination_signals_linux.cc
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
    src/title_bar_win.cc
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
    # Notification click keys (BCryptGenRandom).
    bcrypt
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
  # The secure store (SecItem*, SecAccess, SecTask).
  find_library(SECURITY_FRAMEWORK Security REQUIRED)
  target_link_libraries(laufey_backend_common PUBLIC
    ${USERNOTIFICATIONS_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    ${COCOA_FRAMEWORK}
    ${AUTHENTICATIONSERVICES_FRAMEWORK}
    ${CARBON_FRAMEWORK}
    ${SERVICEMANAGEMENT_FRAMEWORK}
    ${SECURITY_FRAMEWORK}
  )
  target_compile_options(laufey_backend_common PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>
  )
elseif(UNIX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LAUFEY_COMMON_GTK REQUIRED gtk+-3.0)
  # Global shortcuts on X11 (key grabs) and the clipboard on mutter (the
  # CLIPBOARD selection of Xwayland), each over a private XCB connection
  # (libxcb is already a dependency of GTK's X11 backend).
  pkg_check_modules(LAUFEY_COMMON_XCB REQUIRED xcb)
  # The Wayland clipboard without focus (ext-data-control-v1, the client code
  # generated by wayland-scanner in src/wayland/): libwayland-client is
  # already a dependency of GTK's Wayland backend.
  pkg_check_modules(LAUFEY_COMMON_WAYLAND REQUIRED wayland-client)
  target_include_directories(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_INCLUDE_DIRS}
    ${LAUFEY_COMMON_XCB_INCLUDE_DIRS}
    ${LAUFEY_COMMON_WAYLAND_INCLUDE_DIRS}
  )
  # No appindicator build dependency: tray_linux.cc dlopen()s the
  # Ayatana or legacy appindicator library at runtime. CMAKE_DL_LIBS
  # covers dlopen/dlsym on toolchains that still need -ldl.
  target_link_libraries(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_LIBRARIES}
    ${LAUFEY_COMMON_XCB_LIBRARIES}
    ${LAUFEY_COMMON_WAYLAND_LIBRARIES}
    ${CMAKE_DL_LIBS}
  )
  target_compile_options(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_CFLAGS_OTHER}
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The UI exit guard's exit() (src/io_linux.cc) is exported from every
    # executable that links this library, so a library the host loads (the
    # runtime, with dlopen) calls it rather than the C library's.
    target_link_options(laufey_backend_common INTERFACE
      "LINKER:--export-dynamic-symbol=exit"
    )
  endif()
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

# A host's headless launch (`<host> run <script>`: a forked worker, the
# full-app updater's helper) runs its runtime to the end, past the bounded
# wait a windowed app's runtime gets (tests/headless_launch_test.cc, with a
# stand-in runtime library). Each host's CMakeLists (webview, cef) calls this
# with its target and the executable a launch starts.
function(laufey_add_headless_launch_test host_target host_executable)
  if(NOT LAUFEY_BUILD_TESTS)
    return()
  endif()
  add_library(laufey_headless_runtime MODULE
              ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/headless_runtime_lib.cc)
  add_executable(laufey_headless_launch_test
                 ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/headless_launch_test.cc)
  target_compile_features(laufey_headless_launch_test PRIVATE cxx_std_17)
  target_compile_definitions(laufey_headless_launch_test PRIVATE
    LAUFEY_HOST="${host_executable}"
    LAUFEY_HEADLESS_RUNTIME="$<TARGET_FILE:laufey_headless_runtime>"
  )
  add_dependencies(laufey_headless_launch_test
                   ${host_target} laufey_headless_runtime)
  add_test(NAME laufey_headless_launch_test
           COMMAND laufey_headless_launch_test)
  set_tests_properties(laufey_headless_launch_test PROPERTIES TIMEOUT 180)
endfunction()
if(LAUFEY_BUILD_TESTS)
  add_executable(laufey_scheme_registry_test tests/scheme_registry_test.cc)
  target_link_libraries(laufey_scheme_registry_test PRIVATE
    laufey_backend_common
  )
  target_link_libraries(laufey_scheme_registry_test PRIVATE Threads::Threads)
  add_test(NAME laufey_scheme_registry_test
           COMMAND laufey_scheme_registry_test)

  # The bridge's call table (laufey_js_calls.h): backend-issued call ids.
  add_executable(laufey_js_calls_test tests/js_calls_test.cc)
  target_link_libraries(laufey_js_calls_test PRIVATE
    laufey_backend_common
    Threads::Threads
  )
  add_test(NAME laufey_js_calls_test COMMAND laufey_js_calls_test)

  # The calling document's origin and the launch file's bridge pin.
  add_executable(laufey_bridge_origin_test tests/bridge_origin_test.cc)
  target_link_libraries(laufey_bridge_origin_test PRIVATE laufey_backend_common)
  add_test(NAME laufey_bridge_origin_test COMMAND laufey_bridge_origin_test)

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

  # The CEF Linux sandbox decision (and, on Linux, the user-namespace probe).
  add_executable(laufey_cef_sandbox_test tests/cef_sandbox_test.cc)
  target_link_libraries(laufey_cef_sandbox_test PRIVATE
    laufey_backend_common
  )
  add_test(NAME laufey_cef_sandbox_test COMMAND laufey_cef_sandbox_test)

  # The CEF host's network-quiet defaults (features, GAIA origin, Hunspell).
  add_executable(laufey_cef_network_quiet_test tests/cef_network_quiet_test.cc)
  target_link_libraries(laufey_cef_network_quiet_test PRIVATE
    laufey_backend_common
  )
  add_test(NAME laufey_cef_network_quiet_test
           COMMAND laufey_cef_network_quiet_test)

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

  # UTF-8 for text from a JavaScript engine: JSON \u escapes (surrogate
  # pairs, unpaired surrogates) and, on Windows, WideToUtf8 (no OS API
  # elsewhere).
  add_executable(laufey_utf8_test tests/utf8_test.cc)
  target_link_libraries(laufey_utf8_test PRIVATE laufey_backend_common)
  add_test(NAME laufey_utf8_test COMMAND laufey_utf8_test)

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

  # Linux: a library the host loads after main() starts (the runtime) gets
  # the process's argc and argv in its .init_array functions; the host must
  # never change argv (CopyArgvWithoutDBusActivationArg). The probe library
  # reads argv there as Deno's does; the control copy is for the test's
  # negative control.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(probe laufey_argv_probe laufey_argv_probe_control)
      add_library(${probe} MODULE tests/argv_probe_lib.cc)
    endforeach()
    add_executable(laufey_argv_init_array_test tests/argv_init_array_test.cc)
    target_link_libraries(laufey_argv_init_array_test PRIVATE
      laufey_backend_common
      ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(laufey_argv_init_array_test PRIVATE
      LAUFEY_ARGV_PROBE_LIB="$<TARGET_FILE:laufey_argv_probe>"
      LAUFEY_ARGV_PROBE_CONTROL_LIB="$<TARGET_FILE:laufey_argv_probe_control>"
    )
    add_dependencies(laufey_argv_init_array_test
      laufey_argv_probe laufey_argv_probe_control)
    add_test(NAME laufey_argv_init_array_test
             COMMAND laufey_argv_init_array_test
                     --x --laufey-dbus-activated --y)
  endif()

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

  # The decisions made from a platform-features probe: the cookie store,
  # the tray reason, the JSON (no bus).
  add_executable(laufey_platform_features_test tests/platform_features_test.cc)
  target_link_libraries(laufey_platform_features_test PRIVATE
    laufey_backend_common
  )
  add_test(NAME laufey_platform_features_test
           COMMAND laufey_platform_features_test)

  # Title bar preferences: the layout parser, the merge of the sources, the
  # JSON; macOS / Windows: the OS's answer (no bus).
  add_executable(laufey_title_bar_test tests/title_bar_test.cc)
  target_link_libraries(laufey_title_bar_test PRIVATE laufey_backend_common)
  add_test(NAME laufey_title_bar_test COMMAND laufey_title_bar_test)

  if(UNIX AND NOT APPLE)
    # Title bar preferences against a mock xdg-desktop-portal Settings and
    # GSettings' memory backend: the portal wins, SettingChanged is followed
    # live (GTestDBus; skipped without dbus-daemon / glib-compile-schemas).
    add_executable(laufey_title_bar_dbus_test tests/title_bar_dbus_test.cc)
    target_link_libraries(laufey_title_bar_dbus_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_title_bar_dbus_test
             COMMAND laufey_title_bar_dbus_test)
    set_tests_properties(laufey_title_bar_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77
                         ENVIRONMENT "G_DEBUG=fatal-criticals")

    # File dialogs through xdg-desktop-portal's FileChooser against a mock
    # portal (GTestDBus) and GTK's chooser as the fallback, on a private Xvfb
    # (skipped without dbus-daemon / Xvfb).
    add_executable(laufey_file_chooser_dbus_test
                   tests/file_chooser_dbus_test.cc)
    target_link_libraries(laufey_file_chooser_dbus_test PRIVATE
      laufey_backend_common
      Threads::Threads
    )
    add_test(NAME laufey_file_chooser_dbus_test
             COMMAND laufey_file_chooser_dbus_test)
    set_tests_properties(laufey_file_chooser_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77)

    # The secure store: no provider, KWallet without the Secret Service, a
    # provider that never answers, a locked keyring (refused, never "not
    # found"), and a real gnome-keyring-daemon round trip on a private bus
    # (GTestDBus; skipped without dbus-daemon / libsecret).
    add_executable(laufey_secret_store_dbus_test
                   tests/secret_store_dbus_test.cc)
    target_link_libraries(laufey_secret_store_dbus_test PRIVATE
      laufey_backend_common
      Threads::Threads
    )
    add_test(NAME laufey_secret_store_dbus_test
             COMMAND laufey_secret_store_dbus_test)
    set_tests_properties(laufey_secret_store_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77)

    # The Linux platform-features probe against mock services (a tray
    # watcher that comes and goes, a locked / unlocked Secret Service, the
    # portal versions) on a private D-Bus session bus (GTestDBus; skipped
    # without dbus-daemon).
    add_executable(laufey_platform_features_dbus_test
                   tests/platform_features_dbus_test.cc)
    target_link_libraries(laufey_platform_features_dbus_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_platform_features_dbus_test
             COMMAND laufey_platform_features_dbus_test)
    # A GLib critical (the watcher subscription made off the thread that
    # runs the default main context, for one) fails the test.
    set_tests_properties(laufey_platform_features_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77
                         ENVIRONMENT "G_DEBUG=fatal-criticals")

    # Whether a CEF profile's cookie database holds cookies encrypted with
    # the OS key (v11 rows), read with the system SQLite (skipped without
    # libsqlite3).
    add_executable(laufey_cookie_db_test tests/cookie_db_test.cc)
    target_include_directories(laufey_cookie_db_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(laufey_cookie_db_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_cookie_db_test COMMAND laufey_cookie_db_test)
    set_tests_properties(laufey_cookie_db_test PROPERTIES
                         SKIP_RETURN_CODE 77)

    # The freedesktop notification client against a mock server on a
    # private D-Bus session bus (GTestDBus; skipped without dbus-daemon).
    add_executable(laufey_notifications_dbus_test
                   tests/notifications_dbus_test.cc)
    target_link_libraries(laufey_notifications_dbus_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_notifications_dbus_test
             COMMAND laufey_notifications_dbus_test)

    # The portal transport, D-Bus activation (org.freedesktop.Application
    # on the app's name), scheduled launches and systemd user timers
    # against a mock notification server, xdg-desktop-portal and systemd
    # user manager (GTestDBus; skipped without dbus-daemon).
    add_executable(laufey_notifications_portal_dbus_test
                   tests/notifications_portal_dbus_test.cc)
    target_link_libraries(laufey_notifications_portal_dbus_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_notifications_portal_dbus_test
             COMMAND laufey_notifications_portal_dbus_test)
    set_tests_properties(laufey_notifications_portal_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77)

    # What is sent and reported follows the server's GetCapabilities (no
    # "actions", "body-markup"), and the facts' reasons (GTestDBus).
    add_executable(laufey_notifications_caps_dbus_test
                   tests/notifications_caps_dbus_test.cc)
    target_link_libraries(laufey_notifications_caps_dbus_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_notifications_caps_dbus_test
             COMMAND laufey_notifications_caps_dbus_test)
    set_tests_properties(laufey_notifications_caps_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77)

    # The launcher badge (com.canonical.Unity.LauncherEntry) against a mock
    # dock (GTestDBus; skipped without dbus-daemon).
    add_executable(laufey_launcher_entry_dbus_test
                   tests/launcher_entry_dbus_test.cc)
    target_link_libraries(laufey_launcher_entry_dbus_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_launcher_entry_dbus_test
             COMMAND laufey_launcher_entry_dbus_test)
    set_tests_properties(laufey_launcher_entry_dbus_test PROPERTIES
                         SKIP_RETURN_CODE 77)

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

    # Quitting ends the GTK dialogs as a cancel, from the UI thread or
    # another one, nested ones too, and none is shown once quitting (private
    # Xvfb; skipped without it).
    add_executable(laufey_dialog_quit_test tests/dialog_quit_test.cc)
    target_link_libraries(laufey_dialog_quit_test PRIVATE
      laufey_backend_common
      Threads::Threads
    )
    add_test(NAME laufey_dialog_quit_test COMMAND laufey_dialog_quit_test)
    set_tests_properties(laufey_dialog_quit_test PROPERTIES
                         SKIP_RETURN_CODE 77)

    # SIGTERM / SIGINT / SIGHUP: the first quits through the callback on
    # the loop's thread, a second takes the default action.
    add_executable(laufey_termination_signals_test
                   tests/termination_signals_test.cc)
    target_link_libraries(laufey_termination_signals_test PRIVATE
      laufey_backend_common
      Threads::Threads
    )
    add_test(NAME laufey_termination_signals_test
             COMMAND laufey_termination_signals_test)

    # The UI-thread exit guard: exit() from another thread parks the UI
    # thread before the libraries' destructors run.
    # The "late" case's library: exit-time teardown registered after the
    # guard, and an exit() called from a library, as the runtime's is.
    add_library(laufey_exit_guard_late_lib MODULE
                tests/exit_guard_late_lib.cc)
    add_executable(laufey_exit_guard_test tests/exit_guard_test.cc)
    target_link_libraries(laufey_exit_guard_test PRIVATE
      laufey_backend_common
      ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(laufey_exit_guard_test PRIVATE
      LAUFEY_EXIT_GUARD_LATE_LIB="$<TARGET_FILE:laufey_exit_guard_late_lib>"
    )
    add_dependencies(laufey_exit_guard_test laufey_exit_guard_late_lib)
    add_test(NAME laufey_exit_guard_test COMMAND laufey_exit_guard_test)

    # The clipboard through ext-data-control against the session's own
    # compositor (skipped outside a Wayland session with data-control).
    add_executable(laufey_clipboard_wayland_test
                   tests/clipboard_wayland_test.cc)
    target_link_libraries(laufey_clipboard_wayland_test PRIVATE
      laufey_backend_common
    )
    add_test(NAME laufey_clipboard_wayland_test
             COMMAND laufey_clipboard_wayland_test)
    set_tests_properties(laufey_clipboard_wayland_test PROPERTIES
                         SKIP_RETURN_CODE 77)

    # The X11 CLIPBOARD client (the mutter path) against a private Xvfb,
    # with a second X client in the test (skipped without Xvfb).
    add_executable(laufey_clipboard_x11_test tests/clipboard_x11_test.cc)
    target_link_libraries(laufey_clipboard_x11_test PRIVATE
      laufey_backend_common
      Threads::Threads
    )
    add_test(NAME laufey_clipboard_x11_test
             COMMAND laufey_clipboard_x11_test)
    set_tests_properties(laufey_clipboard_x11_test PROPERTIES
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

    # The secure store in the login keychain: round trips, an item another
    # program wrote (never read here, in the way of a store), and that
    # another program can't read this app's items: the reader (a program of
    # its own, prompts off) is refused, and `security find-generic-password
    # -w` doesn't print the secret (LAUFEY_SECRET_TEST_SECURITY_CLI=1: it
    # puts up macOS's prompt on a desktop). Skipped (77) without a usable
    # login keychain unless LAUFEY_SECRET_TEST_REQUIRE=1.
    add_executable(laufey_secret_store_mac_reader
                   tests/secret_store_mac_reader.mm)
    target_link_libraries(laufey_secret_store_mac_reader PRIVATE
      "-framework Foundation" "-framework Security")
    target_compile_options(laufey_secret_store_mac_reader PRIVATE -fobjc-arc)
    add_executable(laufey_secret_store_mac_test
                   tests/secret_store_mac_test.mm)
    target_link_libraries(laufey_secret_store_mac_test PRIVATE
      laufey_backend_common
    )
    target_compile_options(laufey_secret_store_mac_test PRIVATE -fobjc-arc)
    target_compile_definitions(laufey_secret_store_mac_test PRIVATE
      LAUFEY_SECRET_READER="$<TARGET_FILE:laufey_secret_store_mac_reader>")
    add_dependencies(laufey_secret_store_mac_test
                     laufey_secret_store_mac_reader)
    add_test(NAME laufey_secret_store_mac_test
             COMMAND laufey_secret_store_mac_test)
    set_tests_properties(laufey_secret_store_mac_test PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)
  endif()
endif()
