# Build the pinned PyroWave C API into Sunshine.
include_guard(GLOBAL)

# Build the codec and C API without exposing upstream options to Sunshine.
function(sunshine_add_pyrowave)
    set(pyrowave_source_dir "${CMAKE_SOURCE_DIR}/third-party/pyrowave")
    # Defer configuring Granite until the options below are set, including local source overrides.
    CPMAddPackage(NAME pyrowave_granite DOWNLOAD_ONLY YES)

    # Scope upstream options to the dependency build and use its precompiled shaders.
    # cmake-lint: disable=C0103
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(GRANITE_SHARED OFF)
    set(GRANITE_TOOLS OFF)
    set(GRANITE_RENDERER OFF)
    set(GRANITE_PLATFORM "null")
    set(GRANITE_INSTALL_TARGETS OFF)
    set(GRANITE_HIDDEN ON)
    set(GRANITE_POSITION_INDEPENDENT ON)
    set(GRANITE_SHIPPING ON)
    set(GRANITE_FFMPEG OFF)
    set(GRANITE_FFMPEG_VULKAN OFF)
    set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF)
    set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF)
    set(GRANITE_VULKAN_SPIRV_CROSS OFF)
    set(GRANITE_VULKAN_FOSSILIZE OFF)
    set(GRANITE_VULKAN_SYSTEM_HANDLES OFF)
    set(GRANITE_VULKAN_DXGI_INTEROP OFF)
    set(GRANITE_RENDERDOC_CAPTURE OFF)
    add_subdirectory("${pyrowave_granite_SOURCE_DIR}" "${pyrowave_granite_BINARY_DIR}" EXCLUDE_FROM_ALL)

    # Keep Volk's function pointers distinct from the Vulkan loader used by FFmpeg.
    target_compile_definitions(granite-volk-headers INTERFACE VOLK_NAMESPACE)
    set_source_files_properties("${pyrowave_granite_SOURCE_DIR}/third_party/volk/volk.c"
            TARGET_DIRECTORY granite-volk PROPERTIES LANGUAGE CXX)

    set(PYROWAVE_DEVEL OFF)
    set(PYROWAVE_UTILS OFF)
    add_subdirectory("${pyrowave_source_dir}" "${CMAKE_BINARY_DIR}/third-party/pyrowave" EXCLUDE_FROM_ALL)

    # Upstream exposes the C API only for standalone builds; add its wrapper here.
    add_library(sunshine_pyrowave STATIC
            "${pyrowave_source_dir}/pyrowave_c.cpp"
            "${pyrowave_granite_SOURCE_DIR}/video/scaler.cpp")
    target_include_directories(sunshine_pyrowave PRIVATE
            "${pyrowave_source_dir}/shaders" "${pyrowave_granite_SOURCE_DIR}/video")
    target_include_directories(sunshine_pyrowave SYSTEM INTERFACE
            "${pyrowave_source_dir}"
            "${pyrowave_granite_SOURCE_DIR}/third_party/khronos/vulkan-headers/include")
    target_link_libraries(sunshine_pyrowave PRIVATE pyrowave granite-vulkan granite-math Threads::Threads)

    # Clients use the vendored revision to diagnose bitstream compatibility.
    execute_process(COMMAND git -C "${pyrowave_source_dir}" rev-parse --short=8 HEAD
            OUTPUT_VARIABLE pyrowave_bitstream_id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    target_compile_definitions(sunshine_pyrowave INTERFACE
            SUNSHINE_PYROWAVE_BITSTREAM_ID="${pyrowave_bitstream_id}")
endfunction()

sunshine_add_pyrowave()
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES sunshine_pyrowave)
