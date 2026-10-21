cmake_minimum_required(VERSION 3.20)
project(computer-graphics-labs LANGUAGES C CXX)
include(FetchContent)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.5.1)
FetchContent_Declare(vk-bootstrap GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git GIT_TAG v1.4.321)
FetchContent_Declare(VulkanMemoryAllocator GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.4.0)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
set(GLFW_LIBRARY_TYPE STATIC)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_DOCS OFF)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)
FetchContent_MakeAvailable(glfw vk-bootstrap VulkanMemoryAllocator imgui glm)

add_library(graphics_common STATIC common/graphics_internal.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
target_include_directories(graphics_common PUBLIC common ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
if(APPLE)
    # VMA implementation uses nullability annotations that trigger Clang diagnostics internally.
    set_source_files_properties(common/graphics_internal.cpp PROPERTIES COMPILE_OPTIONS -Wno-nullability-completeness)
endif()
target_link_libraries(graphics_common PUBLIC glfw Vulkan::Vulkan vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator glm::glm)

function(add_lab NUMBER)
    set(LAB lab${NUMBER})
    file(GLOB LAB_SOURCES CONFIGURE_DEPENDS labs/${LAB}/source/*.cpp)
    add_executable(${LAB} common/main.cpp ${LAB_SOURCES})
    target_link_libraries(${LAB} PRIVATE graphics_common)
    target_include_directories(${LAB} PRIVATE labs/${LAB}/source)
    target_compile_definitions(${LAB} PRIVATE LAB_NUMBER=${NUMBER}
        LAB_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders/${LAB}"
        GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
    if(MSVC)
        target_compile_options(${LAB} PRIVATE /W4 /wd4201)
        target_compile_definitions(${LAB} PRIVATE _USE_MATH_DEFINES)
    else()
        target_compile_options(${LAB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    endif()
    file(GLOB SHADERS CONFIGURE_DEPENDS labs/${LAB}/shaders/*.vert labs/${LAB}/shaders/*.frag)
    file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS labs/${LAB}/shaders/*.glsl)
    set(BINARIES)
    foreach(SHADER IN LISTS SHADERS)
        get_filename_component(NAME ${SHADER} NAME)
        set(BINARY ${CMAKE_CURRENT_BINARY_DIR}/shaders/${LAB}/${NAME}.spv)
        add_custom_command(OUTPUT ${BINARY}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders/${LAB}
            COMMAND ${GLSLC} --target-env=vulkan1.1 -I${CMAKE_CURRENT_SOURCE_DIR}/labs/${LAB}/shaders ${SHADER} -o ${BINARY}
            DEPENDS ${SHADER} ${SHADER_INCLUDES}
            COMMENT "Compiling ${LAB}/${NAME}"
            VERBATIM)
        list(APPEND BINARIES ${BINARY})
    endforeach()
    add_custom_target(${LAB}_shaders DEPENDS ${BINARIES})
    add_dependencies(${LAB} ${LAB}_shaders)
endfunction()
add_lab(1)
add_lab(2)

if(BUILD_TESTING)
    add_executable(lab2_scene_tests labs/lab2/tests/scene_tests.cpp labs/lab2/source/scene.cpp)
    target_include_directories(lab2_scene_tests PRIVATE labs/lab2/source)
    target_compile_definitions(lab2_scene_tests PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
    target_link_libraries(lab2_scene_tests PRIVATE glm::glm)
    add_test(NAME lab2_scene COMMAND lab2_scene_tests)
endif()
