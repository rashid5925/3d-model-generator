cmake_minimum_required(VERSION 3.20)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()
if(MSVC)
  # Static MSVC runtime: the .exe has no dependency on the VC++ redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(shapeforge VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SHAPEFORGE_VULKAN "Build the Vulkan renderer (loaded at runtime; CPU fallback)" ON)
option(SHAPEFORGE_FETCH_VULKAN_HEADERS "Download Vulkan headers if no SDK is installed" ON)
option(SHAPEFORGE_STATIC_RUNTIME "Link the C++ runtime statically (release binaries)" OFF)
set(SHAPEFORGE_MOLTENVK_STATIC_LIB "" CACHE FILEPATH
    "macOS: path to libMoltenVK.a to bake Vulkan into the executable (auto-detected from the Vulkan SDK)")

add_library(shapeforge_core STATIC
  src/json.cpp
  src/mesh.cpp
  src/simplify.cpp
  src/sdf.cpp
  src/scene.cpp
  src/placement.cpp
  src/image.cpp
  src/inspect.cpp
  src/obj_export.cpp
  src/mcp_server.cpp
  src/platform.cpp
  src/render/render.cpp
  src/render/cpu_renderer.cpp
)
target_include_directories(shapeforge_core PUBLIC src)
# Release builds pass -DSHAPEFORGE_VERSION_OVERRIDE=<tag without v>.
set(SHAPEFORGE_VERSION_STRING "${PROJECT_VERSION}")
if(SHAPEFORGE_VERSION_OVERRIDE)
  set(SHAPEFORGE_VERSION_STRING "${SHAPEFORGE_VERSION_OVERRIDE}")
endif()
target_compile_definitions(shapeforge_core PUBLIC SHAPEFORGE_VERSION="${SHAPEFORGE_VERSION_STRING}")
if(MSVC)
  target_compile_options(shapeforge_core PRIVATE /W4 /utf-8)
  target_compile_definitions(shapeforge_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(shapeforge_core PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(shapeforge_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------- Vulkan ----
if(SHAPEFORGE_VULKAN)
  # Only the headers are needed at build time: the loader is opened at runtime.
  find_path(SHAPEFORGE_VULKAN_INCLUDE vulkan/vulkan.h
            HINTS "$ENV{VULKAN_SDK}/include" "$ENV{VULKAN_SDK}/Include" /usr/local/include /opt/homebrew/include)
  if(NOT SHAPEFORGE_VULKAN_INCLUDE AND SHAPEFORGE_FETCH_VULKAN_HEADERS)
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
      URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.350.tar.gz)
    FetchContent_MakeAvailable(vulkan_headers)
    set(SHAPEFORGE_VULKAN_INCLUDE ${vulkan_headers_SOURCE_DIR}/include)
  endif()

  if(SHAPEFORGE_VULKAN_INCLUDE)
    target_sources(shapeforge_core PRIVATE src/render/vulkan_renderer.cpp src/render/vk_loader.cpp)
    target_include_directories(shapeforge_core PRIVATE ${SHAPEFORGE_VULKAN_INCLUDE})
    target_compile_definitions(shapeforge_core PUBLIC SF_HAVE_VULKAN=1)
    if(NOT MSVC)
      # Vulkan structs are deliberately initialized as {sType} with the rest zeroed.
      set_source_files_properties(src/render/vulkan_renderer.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()

    if(APPLE)
      if(NOT SHAPEFORGE_MOLTENVK_STATIC_LIB)
        find_file(SHAPEFORGE_MOLTENVK_AUTO libMoltenVK.a
                  PATHS "$ENV{VULKAN_SDK}/lib/MoltenVK.xcframework/macos-arm64_x86_64"
                        /usr/local/lib/MoltenVK.xcframework/macos-arm64_x86_64
                  NO_DEFAULT_PATH)
        if(SHAPEFORGE_MOLTENVK_AUTO)
          set(SHAPEFORGE_MOLTENVK_STATIC_LIB ${SHAPEFORGE_MOLTENVK_AUTO})
        endif()
      endif()
      if(SHAPEFORGE_MOLTENVK_STATIC_LIB)
        target_compile_definitions(shapeforge_core PRIVATE SF_STATIC_MOLTENVK=1)
        target_link_libraries(shapeforge_core PUBLIC ${SHAPEFORGE_MOLTENVK_STATIC_LIB}
          "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework IOSurface"
          "-framework IOKit" "-framework CoreGraphics" "-framework AppKit")
        message(STATUS "Vulkan: MoltenVK linked statically (${SHAPEFORGE_MOLTENVK_STATIC_LIB})")
      else()
        message(STATUS "Vulkan: system loader at runtime (no static MoltenVK found)")
      endif()
    else()
      message(STATUS "Vulkan: system loader at runtime")
    endif()

    # Shaders are committed as SPIR-V (src/render/spirv) so building needs no
    # glslc. When glslc is present, edited shaders are recompiled automatically.
    find_program(GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    if(GLSLC_EXECUTABLE)
      set(SPIRV_INCS)
      foreach(stage vert frag)
        set(src ${CMAKE_SOURCE_DIR}/shaders/mesh.${stage})
        set(out ${CMAKE_SOURCE_DIR}/src/render/spirv/mesh.${stage}.inc)
        add_custom_command(OUTPUT ${out}
          COMMAND ${GLSLC_EXECUTABLE} -O -mfmt=c -o ${out} ${src}
          DEPENDS ${src}
          COMMENT "Compiling shader mesh.${stage}")
        list(APPEND SPIRV_INCS ${out})
      endforeach()
      add_custom_target(shapeforge_shaders DEPENDS ${SPIRV_INCS})
      add_dependencies(shapeforge_core shapeforge_shaders)
    endif()
  else()
    message(WARNING "Vulkan headers not found: building with the CPU renderer only")
  endif()
endif()

# ------------------------------------------------------------ executable ----
add_executable(shapeforge src/main.cpp)
target_link_libraries(shapeforge PRIVATE shapeforge_core)
if(SHAPEFORGE_STATIC_RUNTIME AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(shapeforge PRIVATE -static-libstdc++ -static-libgcc)
endif()
if(MINGW)
  target_link_options(shapeforge PRIVATE -static)
endif()
install(TARGETS shapeforge RUNTIME DESTINATION bin)

# ----------------------------------------------------------------- tests ----
enable_testing()
add_executable(shapeforge_tests tests/unit_tests.cpp)
target_link_libraries(shapeforge_tests PRIVATE shapeforge_core)
add_test(NAME unit COMMAND shapeforge_tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME mcp_protocol
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mcp_smoke_test.py $<TARGET_FILE:shapeforge> --renderer cpu)
endif()
