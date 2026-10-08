cmake_minimum_required(VERSION 3.19)
project(laufey_webview VERSION 1.0.0 LANGUAGES C CXX)

# Note: CMAKE_SYSTEM_NAME==iOS also has APPLE==TRUE, so detect iOS first.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  enable_language(OBJCXX)
  set(PLATFORM_IOS TRUE)
elseif(APPLE)
  enable_language(OBJCXX)
  set(PLATFORM_MACOS TRUE)
elseif(UNIX AND NOT APPLE)
  set(PLATFORM_LINUX TRUE)
elseif(WIN32)
  set(PLATFORM_WINDOWS TRUE)
endif()

# Pin the macOS deployment target so the binary's LC_BUILD_VERSION minos matches
# the LSMinimumSystemVersion declared in macos/Info.plist.in. Without this the
# compiler stamps the build machine's SDK version (e.g. 14.0) as the minimum,
# and macOS enforces that Mach-O field at launch — refusing to start on older
# systems regardless of the Info.plist value (denoland/deno#35764). The CEF
# backend gets this for free via cef_variables; the webview backend must set it
# explicitly. Set before add_subdirectory(backend-common) so the shared static
# lib inherits the same target (see backend-common/CMakeLists.txt).
# The webview code uses runtime feature detection (respondsToSelector:,
# valueForKey:/@try) rather than compile-time @available, so a low floor is safe.
if(PLATFORM_MACOS)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING
      "Minimum macOS version" FORCE)
endif()

# Shared native-API implementations (notifications, tray, dock, dialogs, ...).
# Each platform branch links laufey_backend_common below. iOS doesn't use it
# (those are desktop-only AppKit/GTK surfaces); it compiles just the value
# marshalling directly into the target instead.
if(NOT PLATFORM_IOS)
  # Registers backend-common's unit tests with `ctest --test-dir webview/build`.
  enable_testing()
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../backend-common
                   ${CMAKE_BINARY_DIR}/backend-common)

  # The bridge's JSON parser (src/laufey_json.h) against hostile input.
  if(LAUFEY_BUILD_TESTS)
    add_executable(laufey_json_test tests/laufey_json_test.cc)
    target_include_directories(laufey_json_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(laufey_json_test PRIVATE laufey_backend_common)
    target_compile_features(laufey_json_test PRIVATE cxx_std_17)
    add_test(NAME laufey_json_test COMMAND laufey_json_test)
  endif()
endif()

set(LAUFEY_SRCS_COMMON
  src/webview_value.h
  src/runtime_loader.cc
  src/runtime_loader.h
)

if(PLATFORM_IOS)
  set(LAUFEY_SRCS_PLATFORM
    src/webview_ios.mm
    src/main_ios.mm
    # iOS doesn't link backend-common; pull in just the value marshalling.
    ${CMAKE_CURRENT_SOURCE_DIR}/../backend-common/src/laufey_value.cc
  )
elseif(PLATFORM_MACOS)
  set(LAUFEY_SRCS_PLATFORM
    src/webview_macos.mm
    src/main_mac.mm
  )
elseif(PLATFORM_LINUX)
  set(LAUFEY_SRCS_PLATFORM
    src/webview_linux.cc
    src/main_linux.cc
  )
elseif(PLATFORM_WINDOWS)
  set(LAUFEY_SRCS_PLATFORM
    src/webview_windows.cc
    src/main_windows.cc
    src/wv2_scheme_stream.cc
    src/wv2_scheme_stream.h
  )
  # Embed the WebView2 streaming shim (wv2_scheme_stream_shim.js) as adjacent
  # raw string literals; MSVC caps a single literal at 16380 bytes. Re-run on
  # every edit of the script.
  set(_shim_js "${CMAKE_CURRENT_SOURCE_DIR}/src/wv2_scheme_stream_shim.js")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_shim_js}")
  file(READ "${_shim_js}" _shim)
  string(FIND "${_shim}" ")LAUFEYJS\"" _shim_bad)
  if(NOT _shim_bad EQUAL -1)
    message(FATAL_ERROR "wv2_scheme_stream_shim.js contains the raw-string delimiter")
  endif()
  string(LENGTH "${_shim}" _shim_len)
  set(_shim_inc "")
  set(_shim_off 0)
  while(_shim_off LESS _shim_len)
    string(SUBSTRING "${_shim}" ${_shim_off} 12000 _shim_part)
    string(APPEND _shim_inc "R\"LAUFEYJS(${_shim_part})LAUFEYJS\"\n")
    math(EXPR _shim_off "${_shim_off} + 12000")
  endwhile()
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/wv2_scheme_stream_shim.inc"
       CONTENT "${_shim_inc}" @ONLY)
endif()

set(LAUFEY_TARGET "laufey_webview")

if(PLATFORM_IOS)
  # The iOS app links a laufey runtime statically (no dlopen on iOS). Point
  # this at a runtime static lib, e.g. target/aarch64-apple-ios/release/
  # libios_hello.a (built from examples/ios_hello).
  set(LAUFEY_IOS_RUNTIME_LIB "" CACHE FILEPATH "Path to the statically-linked laufey runtime (.a)")

  add_executable(${LAUFEY_TARGET} MACOSX_BUNDLE
    ${LAUFEY_SRCS_COMMON}
    ${LAUFEY_SRCS_PLATFORM}
  )

  find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
  find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)

  target_include_directories(${LAUFEY_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backend-common/include
  )

  target_link_libraries(${LAUFEY_TARGET}
    ${UIKIT_FRAMEWORK}
    ${WEBKIT_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    "-lc++"
  )

  if(LAUFEY_IOS_RUNTIME_LIB)
    # Force the runtime entry points (referenced only from the C++ shell) to be
    # pulled from the static lib.
    target_link_libraries(${LAUFEY_TARGET} "${LAUFEY_IOS_RUNTIME_LIB}")
    target_link_options(${LAUFEY_TARGET} PRIVATE
      "-Wl,-u,_laufey_runtime_init"
      "-Wl,-u,_laufey_runtime_start"
      "-Wl,-u,_laufey_runtime_shutdown"
    )
  endif()

  target_compile_options(${LAUFEY_TARGET} PRIVATE -fobjc-arc)

  set_target_properties(${LAUFEY_TARGET} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.laufey.ios"
    MACOSX_BUNDLE_BUNDLE_NAME "laufey"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
  )

elseif(PLATFORM_MACOS)
  add_executable(${LAUFEY_TARGET} MACOSX_BUNDLE
    ${LAUFEY_SRCS_COMMON}
    ${LAUFEY_SRCS_PLATFORM}
    macos/Info.plist.in
  )

  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
  find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)

  target_link_libraries(${LAUFEY_TARGET}
    ${COCOA_FRAMEWORK}
    ${WEBKIT_FRAMEWORK}
    ${USERNOTIFICATIONS_FRAMEWORK}
    laufey_backend_common
  )

  target_compile_options(${LAUFEY_TARGET} PRIVATE -fobjc-arc)

  set_target_properties(${LAUFEY_TARGET} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.deno.desktop"
    MACOSX_BUNDLE_BUNDLE_NAME "Deno"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
  )

elseif(PLATFORM_LINUX)
  add_executable(${LAUFEY_TARGET}
    ${LAUFEY_SRCS_COMMON}
    ${LAUFEY_SRCS_PLATFORM}
  )

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WEBKITGTK REQUIRED webkit2gtk-4.1)
  # gtk+-unix-print-3.0: the printer lookup behind print_to_pdf (its
  # header only; the symbols are libgtk-3's own).
  pkg_check_modules(GTK REQUIRED gtk+-3.0 gtk+-unix-print-3.0)

  # No appindicator build dependency: the tray in backend-common
  # dlopen()s the Ayatana or legacy appindicator library at runtime.
  target_include_directories(${LAUFEY_TARGET} PRIVATE
    ${WEBKITGTK_INCLUDE_DIRS}
    ${GTK_INCLUDE_DIRS}
  )

  target_link_libraries(${LAUFEY_TARGET}
    ${WEBKITGTK_LIBRARIES}
    ${GTK_LIBRARIES}
    laufey_backend_common
    dl
    pthread
  )

  target_compile_options(${LAUFEY_TARGET} PRIVATE
    ${WEBKITGTK_CFLAGS_OTHER}
    ${GTK_CFLAGS_OTHER}
  )

elseif(PLATFORM_WINDOWS)
  add_executable(${LAUFEY_TARGET} WIN32
    ${LAUFEY_SRCS_COMMON}
    ${LAUFEY_SRCS_PLATFORM}
  )

  if(NOT DEFINED WEBVIEW2_ROOT)
    message(STATUS "WEBVIEW2_ROOT not set, looking for WebView2Loader.dll in system")
  else()
    target_include_directories(${LAUFEY_TARGET} PRIVATE
      ${WEBVIEW2_ROOT}/build/native/include
    )
    # WebView2's NuGet package ships a separate loader import lib per arch under
    # build/native/{x64,x86,arm64}. Pointer size can't tell x64 from arm64 (both
    # 64-bit), so key off the compiler's actual target arch. MSVC_CXX_ARCHITECTURE_ID
    # (X64/X86/ARM64) comes straight from cl.exe's target; fall back to the VS
    # generator platform, then the system processor, then pointer size.
    if(MSVC_CXX_ARCHITECTURE_ID)
      set(_wv2_arch "${MSVC_CXX_ARCHITECTURE_ID}")
    elseif(CMAKE_GENERATOR_PLATFORM)
      set(_wv2_arch "${CMAKE_GENERATOR_PLATFORM}")
    else()
      set(_wv2_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${_wv2_arch}" _wv2_arch)
    if(_wv2_arch MATCHES "arm64|aarch64")
      set(_wv2_arch_dir "arm64")
    elseif(_wv2_arch MATCHES "x64|amd64|x86_64" OR CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_wv2_arch_dir "x64")
    else()
      set(_wv2_arch_dir "x86")
    endif()
    message(STATUS "WebView2 loader arch: ${_wv2_arch_dir}")
    target_link_directories(${LAUFEY_TARGET} PRIVATE
      ${WEBVIEW2_ROOT}/build/native/${_wv2_arch_dir}
    )
  endif()

  target_link_libraries(${LAUFEY_TARGET}
    WebView2LoaderStatic
    user32
    ole32
    oleaut32
    shlwapi
    laufey_backend_common
  )
endif()

set(LAUFEY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../capi/include" CACHE PATH "Path to laufey.h header directory")

target_include_directories(${LAUFEY_TARGET} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${LAUFEY_INCLUDE_DIR}
)
if(PLATFORM_WINDOWS)
  target_include_directories(${LAUFEY_TARGET} PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
  )
endif()

set_target_properties(${LAUFEY_TARGET} PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

# A headless launch (`run <script>`) runs its runtime to the end, on every
# desktop host (backend-common/CMakeLists.txt).
if(NOT PLATFORM_IOS)
  laufey_add_headless_launch_test(${LAUFEY_TARGET}
                                  "$<TARGET_FILE:${LAUFEY_TARGET}>")
endif()
