cmake_minimum_required(VERSION 3.0)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

option(WPE_MESA_GBM "Whether to enable support for the libgbm-based buffer sharing" ON)
option(WPE_MESA_EXPORTABLE_DMA_BUF "Whether to enable support for the exportable dma-buf view backend" ON)

option(WPE_MESA_EXPERIMENTAL_WAYLAND_EGL "Whether to enable support for the wayland-egl nested compositor-based buffer sharing" OFF)

option(WPE_MESA_DRM_TEGRA_SUPPORT "Whether to enable support for the Tegra-specific quirks in the DRM WPE backend" OFF)

find_package(GLIB 2.40.0 REQUIRED COMPONENTS gio gio-unix gobject gthread gmodule)
find_package(LibDRM REQUIRED)
find_package(Libxkbcommon 0.4.0 REQUIRED)
find_package(Wayland 1.6.0 REQUIRED)
find_package(WaylandCursor REQUIRED)
add_definitions(-DWPE_BACKEND_DRM=1)
add_definitions(-DWPE_BACKEND_WAYLAND=1)

pkg_check_modules(WPE wpe REQUIRED)

set(WPE_MESA_INCLUDE_DIRECTORIES
    "src/drm"
    "src/external"
    "src/libxkbcommon"
    "src/util"
    "src/wayland"
    "src/wayland/protocols"
    ${GIO_UNIX_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${LIBXKBCOMMON_INCLUDE_DIRS}
    ${WAYLAND_INCLUDE_DIRS}
    ${WAYLAND_CURSOR_INCLUDE_DIRS}
    ${WPE_INCLUDE_DIRS}
)
set(WPE_MESA_LIBRARIES
    ${GIO_UNIX_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${LIBXKBCOMMON_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${WAYLAND_CURSOR_LIBRARIES}
    ${WPE_LIBRARIES}
)
set(WPE_MESA_SOURCES
    src/mesa.cpp

    src/libxkbcommon/input-libxkbcommon.cpp

    src/util/ipc.cpp

    src/wayland/display.cpp
    src/wayland/pasteboard-wayland.cpp

    src/wayland/protocols/ivi-application-protocol.c
    src/wayland/protocols/wayland-drm-protocol.c
    src/wayland/protocols/xdg-shell-protocol.c
)

if (WPE_MESA_GBM)
    add_definitions(-DWPE_MESA_GBM=1)
    find_package(LibGBM REQUIRED)

    list(APPEND WPE_MESA_INCLUDE_DIRECTORIES
        "src/gbm"
        ${LIBDRM_INCLUDE_DIRS}
        ${LIBGBM_INCLUDE_DIRS}
    )

    list(APPEND WPE_MESA_LIBRARIES
        ${LIBDRM_LIBRARIES}
        ${LIBGBM_LIBRARIES}
    )

    list(APPEND WPE_MESA_SOURCES
        src/drm/view-backend-drm.cpp

        src/gbm/renderer-backend-egl-gbm.cpp
        src/gbm/renderer-host-gbm.cpp

        src/wayland/view-backend-wayland.cpp
    )

  if (WPE_MESA_EXPORTABLE_DMA_BUF)
      list(APPEND WPE_MESA_INCLUDE_DIRECTORIES
          "include"
      )
      set(WPE_MESA_PUBLIC_HEADERS
	  include/wpe-mesa/view-backend-exportable-dma-buf.h
      )
      list(APPEND WPE_MESA_SOURCES
            src/exportable/view-backend-exportable-dma-buf.cpp
      )
    endif ()
endif ()

if (WPE_MESA_EXPERIMENTAL_WAYLAND_EGL)
    add_definitions(-DWPE_MESA_EXPERIMENTAL_WAYLAND_EGL=1)
    find_package(WaylandEGL REQUIRED)

    list(APPEND WPE_MESA_INCLUDE_DIRECTORIES
        "src/nc"
        ${LIBDRM_INCLUDE_DIRS}
        ${WAYLAND_EGL_INCLUDE_DIRS}
    )

    list(APPEND WPE_MESA_LIBRARIES
        ${LIBDRM_LIBRARIES}
        ${WAYLAND_EGL_LIBRARIES}
    )

    list(APPEND WPE_MESA_SOURCES
        src/nc/renderer-backend-egl.cpp
        src/nc/renderer-host.cpp
        src/nc/view-backend-drm.cpp
        src/nc/view-backend-wayland.cpp
        src/nc/nc-view-display.cpp
    )
endif ()

if (WPE_MESA_DRM_TEGRA_SUPPORT)
    add_definitions(-DWPE_BACKEND_DRM_TEGRA=1)
endif ()

link_directories(${WPE_LIBRARY_DIRS})
add_library(WPE-mesa SHARED ${WPE_MESA_SOURCES})
target_include_directories(WPE-mesa PRIVATE ${WPE_MESA_INCLUDE_DIRECTORIES})
target_link_libraries(WPE-mesa ${WPE_MESA_LIBRARIES})

set(WPE_MESA_VERSION_MAJOR 0)
set(WPE_MESA_VERSION_MINOR 1)
set(WPE_MESA_VERSION ${WPE_MESA_VERSION_MAJOR}.${WPE_MESA_VERSION_MINOR})

set_target_properties(WPE-mesa PROPERTIES VERSION ${WPE_MESA_VERSION} SOVERSION ${WPE_MESA_VERSION_MAJOR})
install(TARGETS WPE-mesa EXPORT WPE-mesaTargets
  LIBRARY DESTINATION lib
  INCLUDES DESTINATION include
)

if(WPE_MESA_PUBLIC_HEADERS)
  install(FILES ${WPE_MESA_PUBLIC_HEADERS} DESTINATION include/wpe-mesa)
endif()

# Create a libWPE-backend.so symlink to libWPE-mesa.so and install it.
add_custom_command(TARGET WPE-mesa
                   POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink libWPE-mesa.so ${CMAKE_BINARY_DIR}/libWPE-backend.so
)
install(FILES ${CMAKE_BINARY_DIR}/libWPE-backend.so DESTINATION lib)

configure_file(wpe-mesa.pc.cmake wpe-mesa.pc @ONLY)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/wpe-mesa.pc"
    DESTINATION
    lib/pkgconfig
)
