cmake_minimum_required(VERSION 3.20)

project(QF-ML)

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()


# ============================================================================
# Qt
# ============================================================================

find_package(Qt6 REQUIRED COMPONENTS
        Core
        Gui
        Widgets
        Network
        Multimedia
        WebEngineWidgets
        WebEngineCore
        WebChannel
        TextToSpeech
)

qt_standard_project_setup()

# =============================================================================
# 1. Whisper.cpp
# =============================================================================

include(FetchContent)

FetchContent_Declare(
        whisper
        GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
        GIT_TAG v1.7.1
)

set(
        WHISPER_BUILD_EXAMPLES
        OFF
        CACHE BOOL ""
        FORCE
)
set(
        WHISPER_BUILD_TESTS
        OFF
        CACHE BOOL ""
        FORCE
)

# Whisper's GGML must not be a shared library in the QF-ML process.
#
# Parakeet contains its own GGML implementation. Having both implementations
# exported as shared/global symbols causes gguf_* calls from Parakeet to resolve
# to Whisper's GGML.
set(
        BUILD_SHARED_LIBS
        OFF
        CACHE BOOL ""
        FORCE
)

set(
        GGML_BACKEND_DL
        OFF
        CACHE BOOL ""
        FORCE
)

if (APPLE)

    set(
            WHISPER_METAL
            ON
            CACHE BOOL ""
            FORCE
    )

endif ()

FetchContent_MakeAvailable(whisper)

# =============================================================================
# 2. Parakeet.cpp
# =============================================================================
#
# Parakeet MUST remain an ExternalProject.
#
# It vendors its own GGML and cannot safely be added directly to the QF-ML
# CMake target graph because Whisper also vendors GGML.
#
# Parakeet's own CMakeLists.txt links GGML as PUBLIC, which re-exports GGML's
# symbols (including Vulkan backend registration symbols) from libparakeet.so.
# When that happens alongside Whisper's statically-linked GGML in the same
# process, symbol interposition/duplicate-registration issues can occur.
#
# Parakeet is a submodule and is not writable in-tree, so the fix is applied
# via PATCH_COMMAND against the checked-out submodule source immediately
# before ExternalProject configures it. This flips ggml linkage from PUBLIC
# to PRIVATE. It does not touch any other line and is idempotent (sed with
# a no-op match on subsequent re-configures).
# =============================================================================

include(ExternalProject)

set(
        PARAKEET_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/external/parakeet.cpp"
)

if (NOT EXISTS "${PARAKEET_SOURCE_DIR}/CMakeLists.txt")

    message(
            FATAL_ERROR
            "Parakeet.cpp not found at ${PARAKEET_SOURCE_DIR}"
    )

endif ()

set(
        PARAKEET_BUILD_DIR
        "${CMAKE_BINARY_DIR}/parakeet-build"
)

set(
        PARAKEET_INCLUDE_DIR
        "${PARAKEET_SOURCE_DIR}/include"
)

if (WIN32)

    set(
            PARAKEET_LIBRARY
            "${PARAKEET_BUILD_DIR}/parakeet.dll"
    )

elseif (APPLE)

    set(
            PARAKEET_LIBRARY
            "${PARAKEET_BUILD_DIR}/libparakeet.dylib"
    )

else ()

    set(
            PARAKEET_LIBRARY
            "${PARAKEET_BUILD_DIR}/libparakeet.so"
    )

endif ()
# -----------------------------------------------------------------------------
# Patch Parakeet's vendored GGML for older Vulkan-Headers
# -----------------------------------------------------------------------------
#
# Debian 12 ships Vulkan-Headers 1.3.239, which does not provide the Vulkan-Hpp
# types for VK_EXT_layer_settings:
#
#   vk::LayerSettingEXT
#   vk::LayerSettingTypeEXT
#   vk::LayerSettingsCreateInfoEXT
#
# The GGML Vulkan backend uses these types unconditionally even though the
# extension itself is optional at runtime.
#
# This patch wraps the layer-settings code in #ifdef VK_EXT_layer_settings so
# GGML Vulkan can still build and run without that optional extension.
#
# The patch is applied through ExternalProject_Add(), so the git submodule
# itself does not need to be edited manually.
# -----------------------------------------------------------------------------

set(
        PARAKEET_VULKAN_PATCH_SCRIPT
        "${CMAKE_BINARY_DIR}/patch_parakeet_vulkan.cmake"
)

file(
        WRITE
        "${PARAKEET_VULKAN_PATCH_SCRIPT}"
        [=[
file(READ "${PARAKEET_SOURCE_DIR}/third_party/ggml/src/ggml-vulkan/ggml-vulkan.cpp" _content)

# ---------------------------------------------------------------------------
# 1. Guard extension advertisement.
# ---------------------------------------------------------------------------

string(REPLACE
"    if (layer_settings) {
        extensions.push_back(\"VK_EXT_layer_settings\");
    }"
"#ifdef VK_EXT_layer_settings
    if (layer_settings) {
        extensions.push_back(\"VK_EXT_layer_settings\");
    }
#endif"
_content
"${_content}"
)

# ---------------------------------------------------------------------------
# 2. Replace unconditional LayerSettings Vulkan-Hpp usage with a fallback.
# ---------------------------------------------------------------------------

string(REPLACE
"    VkBool32 enable_best_practice = layer_settings;
    std::vector<vk::LayerSettingEXT> settings = {
        {
            \"VK_LAYER_KHRONOS_validation\",
            \"validate_best_practices\",
            vk::LayerSettingTypeEXT::eBool32,
            1,
            &enable_best_practice
        },
    };
    vk::LayerSettingsCreateInfoEXT layer_setting_info(settings);
    vk::InstanceCreateInfo instance_create_info(vk::InstanceCreateFlags{}, &app_info, layers, extensions, &layer_setting_info);"
"#ifdef VK_EXT_layer_settings
    VkBool32 enable_best_practice = layer_settings;
    std::vector<vk::LayerSettingEXT> settings = {
        {
            \"VK_LAYER_KHRONOS_validation\",
            \"validate_best_practices\",
            vk::LayerSettingTypeEXT::eBool32,
            1,
            &enable_best_practice
        },
    };
    vk::LayerSettingsCreateInfoEXT layer_setting_info(settings);
    vk::InstanceCreateInfo instance_create_info(
        vk::InstanceCreateFlags{},
        &app_info,
        layers,
        extensions,
        &layer_setting_info
    );
#else
    vk::InstanceCreateInfo instance_create_info(
        vk::InstanceCreateFlags{},
        &app_info,
        layers,
        extensions
    );
#endif"
_content
"${_content}"
)

file(
        WRITE
        "${PARAKEET_SOURCE_DIR}/third_party/ggml/src/ggml-vulkan/ggml-vulkan.cpp"
        "${_content}"
)
]=]
)

ExternalProject_Add(
        parakeet_external

        SOURCE_DIR
        "${PARAKEET_SOURCE_DIR}"

        BINARY_DIR
        "${PARAKEET_BUILD_DIR}"

        PATCH_COMMAND
        ${CMAKE_COMMAND}
        -DPARAKEET_SOURCE_DIR=${PARAKEET_SOURCE_DIR}
        -P
        "${PARAKEET_VULKAN_PATCH_SCRIPT}"

        CMAKE_ARGS

        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"

        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"

        # Build shared Parakeet library.
        "-DPARAKEET_SHARED=ON"

        "-DPARAKEET_BUILD_TESTS=OFF"
        "-DPARAKEET_BUILD_CLI=OFF"
        "-DPARAKEET_BUILD_SERVER=OFF"

        # Keep vendored GGML static.
        "-DBUILD_SHARED_LIBS=OFF"

        # Do not dynamically load GGML backends.
        "-DGGML_BACKEND_DL=OFF"

        # GPU / Vulkan.
        "-DPARAKEET_GGML_VULKAN=ON"
        "-DGGML_VULKAN=ON"

        BUILD_COMMAND
        ${CMAKE_COMMAND}
        --build
        "${PARAKEET_BUILD_DIR}"
        --config
        "${CMAKE_BUILD_TYPE}"

        INSTALL_COMMAND
        ""

        BUILD_BYPRODUCTS
        "${PARAKEET_LIBRARY}"

        BUILD_ALWAYS
        OFF

        UPDATE_DISCONNECTED
        TRUE
)
# -----------------------------------------------------------------------------
# Imported Parakeet target
# -----------------------------------------------------------------------------

add_library(
        Parakeet::Parakeet
        SHARED
        IMPORTED
        GLOBAL
)

set_target_properties(
        Parakeet::Parakeet

        PROPERTIES

        IMPORTED_LOCATION
        "${PARAKEET_LIBRARY}"

        INTERFACE_INCLUDE_DIRECTORIES
        "${PARAKEET_INCLUDE_DIR}"
)

add_dependencies(
        Parakeet::Parakeet
        parakeet_external
)

# =============================================================================
# 3. ONNX Runtime
# =============================================================================

set(
        ONNXRUNTIME_VERSION
        "1.17.1"
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")

    set(
            ONNX_URL
            "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz"
    )

elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")

    if (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")

        set(
                ONNX_URL
                "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-osx-arm64-${ONNXRUNTIME_VERSION}.tgz"
        )

    else ()

        set(
                ONNX_URL
                "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-osx-x64-${ONNXRUNTIME_VERSION}.tgz"
        )

    endif ()

elseif (CMAKE_SYSTEM_NAME STREQUAL "Windows")

    set(
            ONNX_URL
            "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip"
    )

else ()

    message(
            FATAL_ERROR
            "Unsupported platform: ${CMAKE_SYSTEM_NAME}"
    )

endif ()

FetchContent_Declare(
        onnxruntime_prebuilt
        URL ${ONNX_URL}
)

FetchContent_MakeAvailable(
        onnxruntime_prebuilt
)

set(
        ONNXRUNTIME_INCLUDE_DIR
        "${onnxruntime_prebuilt_SOURCE_DIR}/include"
)

if (WIN32)

    set(
            ONNXRUNTIME_LIBRARY
            "${onnxruntime_prebuilt_SOURCE_DIR}/lib/onnxruntime.lib"
    )

    set(
            ONNXRUNTIME_DLL
            "${onnxruntime_prebuilt_SOURCE_DIR}/lib/onnxruntime.dll"
    )

elseif (APPLE)

    set(
            ONNXRUNTIME_LIBRARY
            "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.dylib"
    )

    file(
            GLOB
            ONNXRUNTIME_DYLIBS
            "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime*.dylib"
    )

else ()

    set(
            ONNXRUNTIME_LIBRARY
            "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.so"
    )

    file(
            GLOB
            ONNXRUNTIME_SOLIBS
            "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.so*"
    )

endif ()

add_library(
        ONNXRuntime::ONNXRuntime
        UNKNOWN
        IMPORTED
)

set_target_properties(
        ONNXRuntime::ONNXRuntime

        PROPERTIES

        INTERFACE_INCLUDE_DIRECTORIES
        "${ONNXRUNTIME_INCLUDE_DIR}"

        IMPORTED_LOCATION
        "${ONNXRUNTIME_LIBRARY}"
)

# =============================================================================
# 4. Model Downloader
# =============================================================================

set(
        MODELS_DIR
        "${CMAKE_BINARY_DIR}/models"
)

file(
        MAKE_DIRECTORY
        "${MODELS_DIR}"
)

# -----------------------------------------------------------------------------
# Hugging Face CLI detection
# -----------------------------------------------------------------------------

find_program(
        HF_CLI
        NAMES
        hf
)

if (HF_CLI)

    message(
            STATUS
            "Hugging Face CLI found: ${HF_CLI}"
    )

    set(
            HF_AVAILABLE
            TRUE
    )

else ()

    message(
            STATUS
            "Hugging Face CLI not found."
    )

    message(
            STATUS
            "Hugging Face downloads will use CMake HTTP fallback."
    )

    set(
            HF_AVAILABLE
            FALSE
    )

endif ()

# -----------------------------------------------------------------------------
# Generic downloader
# -----------------------------------------------------------------------------

function(
        download_model
        url
        dest
        description
)

    set(
            should_download
            FALSE
    )

    if (NOT EXISTS "${dest}")

        set(
                should_download
                TRUE
        )

    else ()

        file(
                SIZE
                "${dest}"
                file_size
        )

        if (file_size EQUAL 0)

            set(
                    should_download
                    TRUE
            )

        endif ()

    endif ()

    if (should_download)

        message(
                STATUS
                ""
        )

        message(
                STATUS
                "Downloading ${description}..."
        )

        message(
                STATUS
                "URL: ${url}"
        )

        file(
                DOWNLOAD
                "${url}"
                "${dest}"

                SHOW_PROGRESS

                STATUS
                download_status

                TIMEOUT
                1800

                INACTIVITY_TIMEOUT
                120
        )

        list(
                GET
                download_status
                0
                status_code
        )

        list(
                GET
                download_status
                1
                error_message
        )

        if (NOT status_code EQUAL 0)

            file(
                    REMOVE
                    "${dest}"
            )

            message(
                    FATAL_ERROR
                    "Failed to download ${description}: ${error_message}\n"
                    "URL: ${url}"
            )

        endif ()

        file(
                SIZE
                "${dest}"
                file_size
        )

        if (file_size EQUAL 0)

            file(
                    REMOVE
                    "${dest}"
            )

            message(
                    FATAL_ERROR
                    "${description} downloaded as an empty file."
            )

        endif ()

        message(
                STATUS
                "Successfully downloaded ${description} "
                "(${file_size} bytes)"
        )

    else ()

        file(
                SIZE
                "${dest}"
                file_size
        )

        message(
                STATUS
                "${description} already exists (${file_size} bytes)"
        )

    endif ()

endfunction()

# -----------------------------------------------------------------------------
# Hugging Face downloader
#
# When hf is available:
#
#   HF_XET_HIGH_PERFORMANCE=1
#
# is used for large model transfers.
# -----------------------------------------------------------------------------

function(
        download_hf_model
        repo_id
        filename
        destination
        description
)

    get_filename_component(
            destination_dir
            "${destination}"
            DIRECTORY
    )

    file(
            MAKE_DIRECTORY
            "${destination_dir}"
    )

    if (EXISTS "${destination}")

        file(
                SIZE
                "${destination}"
                existing_size
        )

        if (existing_size GREATER 0)

            message(
                    STATUS
                    "${description} already exists "
                    "(${existing_size} bytes)"
            )

            return()

        endif ()

        file(
                REMOVE
                "${destination}"
        )

    endif ()

    if (HF_AVAILABLE)

        message(
                STATUS
                ""
        )

        message(
                STATUS
                "Downloading ${description} using Hugging Face Xet..."
        )

        execute_process(
                COMMAND
                ${CMAKE_COMMAND}
                -E
                env

                HF_XET_HIGH_PERFORMANCE=1
                HF_HUB_DOWNLOAD_TIMEOUT=1800

                ${HF_CLI}
                download
                "${repo_id}"
                "${filename}"
                --local-dir
                "${destination_dir}"
                --max-workers
                "8"

                RESULT_VARIABLE
                hf_result

                OUTPUT_VARIABLE
                hf_output

                ERROR_VARIABLE
                hf_error

                COMMAND_ECHO
                STDOUT
        )

        if (NOT hf_result EQUAL 0)

            message(
                    WARNING
                    "Hugging Face download failed for ${description}."
            )

            message(
                    WARNING
                    "stdout:\n${hf_output}"
            )

            message(
                    WARNING
                    "stderr:\n${hf_error}"
            )

            message(
                    STATUS
                    "Falling back to CMake HTTP download..."
            )

            download_model(
                    "https://huggingface.co/${repo_id}/resolve/main/${filename}"
                    "${destination}"
                    "${description}"
            )

        endif ()

    else ()

        download_model(
                "https://huggingface.co/${repo_id}/resolve/main/${filename}"
                "${destination}"
                "${description}"
        )

    endif ()

    if (NOT EXISTS "${destination}")

        message(
                FATAL_ERROR
                "Expected downloaded file was not created:\n"
                "${destination}"
        )

    endif ()

    file(
            SIZE
            "${destination}"
            downloaded_size
    )

    if (downloaded_size EQUAL 0)

        file(
                REMOVE
                "${destination}"
        )

        message(
                FATAL_ERROR
                "${description} is empty."
        )

    endif ()

    message(
            STATUS
            "${description}: ${downloaded_size} bytes"
    )

endfunction()

# =============================================================================
# 4A. Whisper model
# =============================================================================

set(
        MODEL_NAME
        "ggml-tiny.en.bin"
)

set(
        MODEL_PATH
        "${CMAKE_BINARY_DIR}/${MODEL_NAME}"
)

download_hf_model(
        "ggerganov/whisper.cpp"
        "${MODEL_NAME}"
        "${MODEL_PATH}"
        "Whisper model ${MODEL_NAME}"
)

# =============================================================================
# 4B. Parakeet model
# =============================================================================

set(
        PARAKEET_MODEL_NAME
        "tdt-1.1b-f16.gguf"
)

set(
        PARAKEET_MODEL_PATH
        "${MODELS_DIR}/${PARAKEET_MODEL_NAME}"
)

# Approximately 2.4 GB.
# Reject the previously encountered ~1.2 GB truncated download.
set(
        PARAKEET_MODEL_MIN_SIZE
        2400000000
)

set(
        PARAKEET_MODEL_VALID
        FALSE
)

if (EXISTS "${PARAKEET_MODEL_PATH}")

    file(
            SIZE
            "${PARAKEET_MODEL_PATH}"
            PARAKEET_EXISTING_SIZE
    )

    if (
            PARAKEET_EXISTING_SIZE
            GREATER_EQUAL
            PARAKEET_MODEL_MIN_SIZE
    )

        set(
                PARAKEET_MODEL_VALID
                TRUE
        )

        message(
                STATUS
                "Parakeet model already exists:"
                "${PARAKEET_MODEL_PATH}"
                "(${PARAKEET_EXISTING_SIZE} bytes)"
        )

    else ()

        message(
                WARNING
                "Existing Parakeet model appears incomplete:"
                "${PARAKEET_MODEL_PATH}"
                "(${PARAKEET_EXISTING_SIZE} bytes)"
        )

        file(
                REMOVE
                "${PARAKEET_MODEL_PATH}"
        )

    endif ()

endif ()

if (NOT PARAKEET_MODEL_VALID)

    download_hf_model(
            "mudler/parakeet-cpp-gguf"
            "${PARAKEET_MODEL_NAME}"
            "${PARAKEET_MODEL_PATH}"
            "Parakeet TDT 1.1B F16 model"
    )

    file(
            SIZE
            "${PARAKEET_MODEL_PATH}"
            PARAKEET_DOWNLOADED_SIZE
    )

    if (
            PARAKEET_DOWNLOADED_SIZE
            LESS
            PARAKEET_MODEL_MIN_SIZE
    )

        file(
                REMOVE
                "${PARAKEET_MODEL_PATH}"
        )

        message(
                FATAL_ERROR
                "Parakeet model download is incomplete.\n"
                "Downloaded: ${PARAKEET_DOWNLOADED_SIZE} bytes\n"
                "Expected at least: ${PARAKEET_MODEL_MIN_SIZE} bytes"
        )

    endif ()

endif ()

# =============================================================================
# 4C. openWakeWord models
# =============================================================================

set(
        OWW_URLS

        "https://github.com/dscripka/openWakeWord/releases/download/v0.5.1/melspectrogram.onnx"

        "https://github.com/dscripka/openWakeWord/releases/download/v0.5.1/embedding_model.onnx"

        "https://github.com/dscripka/openWakeWord/releases/download/v0.5.1/hey_jarvis_v0.1.onnx"
)

set(
        OWW_FILES

        "melspectrogram.onnx"

        "embedding_model.onnx"

        "hey_jarvis.onnx"
)

list(
        LENGTH
        OWW_FILES
        NUM_OWW_MODELS
)

math(
        EXPR
        MAX_OWW_INDEX
        "${NUM_OWW_MODELS} - 1"
)

foreach (
        INDEX
        RANGE
        ${MAX_OWW_INDEX}
)

    list(
            GET
            OWW_FILES
            ${INDEX}
            MODEL_FILE
    )

    list(
            GET
            OWW_URLS
            ${INDEX}
            FILE_URL
    )

    set(
            DEST_PATH
            "${MODELS_DIR}/${MODEL_FILE}"
    )

    download_model(
            "${FILE_URL}"
            "${DEST_PATH}"
            "openWakeWord model ${MODEL_FILE}"
    )

endforeach ()

# =============================================================================
# 5. OCR Libraries
# =============================================================================

find_package(
        PkgConfig
        QUIET
)

if (PkgConfig_FOUND)

    pkg_check_modules(
            TESSERACT
            tesseract
    )

    pkg_check_modules(
            LEPTONICA
            lept
    )

endif ()

if (NOT TESSERACT_FOUND)

    find_library(
            TESSERACT_LIBRARIES
            NAMES
            tesseract
    )

    find_path(
            TESSERACT_INCLUDE_DIRS
            NAMES
            tesseract/baseapi.h
    )

endif ()

if (NOT LEPTONICA_FOUND)

    find_library(
            LEPTONICA_LIBRARIES
            NAMES
            lept
            leptonica
    )

    find_path(
            LEPTONICA_INCLUDE_DIRS
            NAMES
            leptonica/allheaders.h
    )

endif ()

# =============================================================================
# 6. QF-ML executable
# =============================================================================

qt_add_executable(
        QF-ML

        src/main.cpp


        include/inference/LlmClient.h
        src/inference/LlmClient.cpp

        include/inference/InferenceService.h
        src/inference/InferenceService.cpp

        include/vision/CaptureOverlay.h
        src/vision/CaptureOverlay.cpp

        include/voice/AudioRecorder.h

        include/voice/WhisperTranscriber.h

        include/voice/ParakeetTranscriber.h
        src/voice/ParakeetTranscriber.cpp

        include/voice/ITranscriber.h

        include/voice/WakeWordDetector.h

        include/vision/OcrService.h
        src/vision/OcrService.cpp

        src/voice/TtsManager.cpp
        include/voice/TtsManager.h

        src/inference/LlamaManager.cpp
        include/inference/LlamaManager.h

        src/inference/ModelManager.cpp
        include/inference/ModelManager.h

        src/ui/ModelDialog.cpp
        include/ui/ModelDialog.h

        include/ui/QuantizationGuideDialog.h
        src/ui/QuantizationGuideDialog.cpp

        include/app/RuntimeManager.h
        src/app/RuntimeManager.cpp
)


target_include_directories(
        QF-ML

        PRIVATE

        ${TESSERACT_INCLUDE_DIRS}

        ${LEPTONICA_INCLUDE_DIRS}

        ${whisper_SOURCE_DIR}

        "${PARAKEET_INCLUDE_DIR}"

        ${ONNXRUNTIME_INCLUDE_DIR}

        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# =============================================================================
# 9. Libraries
# =============================================================================

target_link_libraries(
        QF-ML

        PRIVATE

        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Multimedia
        Qt6::WebEngineWidgets
        Qt6::WebEngineCore
        Qt6::WebChannel
        Qt6::TextToSpeech

        whisper

        Parakeet::Parakeet

        ONNXRuntime::ONNXRuntime

        ${TESSERACT_LIBRARIES}

        ${LEPTONICA_LIBRARIES}
)

if (UNIX AND NOT APPLE)

    target_link_options(
            QF-ML
            PRIVATE
            "-Wl,--exclude-libs,ALL"
    )

endif ()

if (UNIX AND NOT APPLE)

    set_target_properties(
            QF-ML

            PROPERTIES

            BUILD_RPATH
            "$ORIGIN;${onnxruntime_prebuilt_SOURCE_DIR}/lib"

            INSTALL_RPATH
            "$ORIGIN"
    )

elseif (APPLE)

    set_target_properties(
            QF-ML

            PROPERTIES

            BUILD_RPATH
            "@loader_path;${onnxruntime_prebuilt_SOURCE_DIR}/lib"

            INSTALL_RPATH
            "@loader_path"
    )

endif ()


add_custom_command(
        TARGET QF-ML
        POST_BUILD

        COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        "$<TARGET_FILE_DIR:QF-ML>/models"

        COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_directory
        "${MODELS_DIR}"
        "$<TARGET_FILE_DIR:QF-ML>/models"

        COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_if_different
        "${MODEL_PATH}"
        "$<TARGET_FILE_DIR:QF-ML>/${MODEL_NAME}"
)

if (WIN32)

    add_custom_command(
            TARGET QF-ML
            POST_BUILD

            COMMAND
            ${CMAKE_COMMAND}
            -E
            copy_if_different
            "${ONNXRUNTIME_DLL}"
            "$<TARGET_FILE_DIR:QF-ML>/onnxruntime.dll"
    )

elseif (APPLE)

    foreach (
            dylib
            ${ONNXRUNTIME_DYLIBS}
    )

        add_custom_command(
                TARGET QF-ML
                POST_BUILD

                COMMAND
                ${CMAKE_COMMAND}
                -E
                copy_if_different
                "${dylib}"
                "$<TARGET_FILE_DIR:QF-ML>/"
        )

    endforeach ()

else ()

    foreach (
            solib
            ${ONNXRUNTIME_SOLIBS}
    )

        add_custom_command(
                TARGET QF-ML
                POST_BUILD

                COMMAND
                ${CMAKE_COMMAND}
                -E
                copy_if_different
                "${solib}"
                "$<TARGET_FILE_DIR:QF-ML>/"
        )

    endforeach ()

endif ()


add_custom_command(
        TARGET QF-ML
        POST_BUILD

        COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_if_different
        "$<TARGET_FILE:Parakeet::Parakeet>"
        "$<TARGET_FILE_DIR:QF-ML>"
)


add_custom_command(
        TARGET QF-ML
        POST_BUILD

        COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        "$<TARGET_FILE_DIR:QF-ML>/models"

        COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_if_different
        "${PARAKEET_MODEL_PATH}"
        "$<TARGET_FILE_DIR:QF-ML>/models/${PARAKEET_MODEL_NAME}"
)


add_dependencies(
        QF-ML
        Parakeet::Parakeet
)