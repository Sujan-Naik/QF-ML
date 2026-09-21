cmake_minimum_required(VERSION 3.26)

project(
        QF-ML
        LANGUAGES CXX
)

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

        WebChannel
        TextToSpeech
)

qt_standard_project_setup()

include(FetchContent)

# =============================================================================
# 1. NeMo-Speech.cpp
# =============================================================================
#
# This project uses NeMo-Speech.cpp as its speech runtime.
#
# Removed completely:
#   - whisper.cpp
#   - parakeet.cpp
#
# NeMo provides:
#   - ASR
#   - stable C ABI
#   - ggml runtime
#   - Vulkan backend
#
# We only build ASR.
# =============================================================================

set(
        NEMO_SPEECH_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/external/NeMo-Speech.cpp"
)

if (NOT EXISTS
        "${NEMO_SPEECH_SOURCE_DIR}/CMakeLists.txt"
)

    message(
            FATAL_ERROR
            "NeMo-Speech.cpp was not found at:\n"
            "${NEMO_SPEECH_SOURCE_DIR}\n\n"
            "Run:\n"
            "  git submodule update --init --recursive"
    )

endif ()

if (NOT EXISTS
        "${NEMO_SPEECH_SOURCE_DIR}/include/nemo_speech/asr.h"
)

    message(
            FATAL_ERROR
            "NeMo-Speech.cpp ASR header was not found:\n"
            "${NEMO_SPEECH_SOURCE_DIR}/include/nemo_speech/asr.h\n\n"
            "Run:\n"
            "  git submodule update --init --recursive"
    )

endif ()

# -----------------------------------------------------------------------------
# NeMo component selection
# -----------------------------------------------------------------------------

set(
        NEMO_SPEECH_BUILD_ASR
        ON
        CACHE BOOL
        "Build NeMo ASR"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_DIAR
        OFF
        CACHE BOOL
        "Build standalone/ASR-integrated diarization"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_TTS
        OFF
        CACHE BOOL
        "Build NeMo TTS"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_NMT
        OFF
        CACHE BOOL
        "Build NeMo NMT"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_CLI
        OFF
        CACHE BOOL
        "Build NeMo CLI"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_MIC_CAPTURE
        OFF
        CACHE BOOL
        "Build microphone capture"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_HTTP
        OFF
        CACHE BOOL
        "Build HTTP server"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_GRPC
        OFF
        CACHE BOOL
        "Build gRPC"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_TESTS
        OFF
        CACHE BOOL
        "Build tests"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_EXAMPLES
        OFF
        CACHE BOOL
        "Build examples"
        FORCE
)

set(
        NEMO_SPEECH_BUILD_TOOLS
        OFF
        CACHE BOOL
        "Build developer tools"
        FORCE
)

set(
        NEMO_SPEECH_WITH_FLASHLIGHT
        OFF
        CACHE BOOL
        "Build Flashlight/KenLM decoder"
        FORCE
)

set(
        NEMO_SPEECH_WITH_NORM
        OFF
        CACHE BOOL
        "Build text normalization"
        FORCE
)

set(
        NEMO_SPEECH_TTS_WITH_JA
        OFF
        CACHE BOOL
        ""
        FORCE
)

set(
        NEMO_SPEECH_TTS_WITH_ZH
        OFF
        CACHE BOOL
        ""
        FORCE
)

# -----------------------------------------------------------------------------
# ggml backend
# -----------------------------------------------------------------------------

set(
        GGML_VULKAN
        ON
        CACHE BOOL
        "Enable ggml Vulkan backend"
        FORCE
)

set(
        GGML_METAL
        OFF
        CACHE BOOL
        "Enable ggml Metal backend"
        FORCE
)

set(
        NEMO_SPEECH_GGML_PATCHED
        OFF
        CACHE BOOL
        "Use patched NeMo ggml"
        FORCE
)

# =============================================================================
# 1A. Embedded NeMo include path
# =============================================================================
#
# NeMo's current src/asr/CMakeLists.txt gives nemo_speech_asr_c:
#
#   $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
#
# When embedded in QF-ML, CMAKE_SOURCE_DIR refers to QF-ML.
#
# c_api.cpp includes:
#
#   #include "nemo_speech/asr.h"
#
# Therefore NeMo's public include directory must be made visible from the
# parent directory before add_subdirectory().
# =============================================================================

include_directories(
        BEFORE
        "${NEMO_SPEECH_SOURCE_DIR}/include"
)

# =============================================================================
# 1B. Vulkan-Headers compatibility patch
# =============================================================================
#
# Your Debian installation provides Vulkan headers that predate the
# VK_EXT_layer_settings Vulkan-Hpp declarations used by the vendored ggml
# Vulkan backend.
#
# The following symbols fail to compile on older Vulkan-Hpp:
#
#   vk::LayerSettingEXT
#   vk::LayerSettingTypeEXT
#   vk::LayerSettingsCreateInfoEXT
#
# The extension is optional. When the headers do not expose it, use ordinary
# vk::InstanceCreateInfo without the pNext chain.
#
# The patch is applied only when the problematic source is present, so once
# NVIDIA removes the incompatibility this CMake becomes a no-op automatically.
# =============================================================================

set(
        NEMO_VULKAN_CPP
        "${NEMO_SPEECH_SOURCE_DIR}/ggml/src/ggml-vulkan/ggml-vulkan.cpp"
)

if (EXISTS "${NEMO_VULKAN_CPP}")

    file(
            READ
            "${NEMO_VULKAN_CPP}"
            NEMO_VULKAN_CONTENT
    )

    if (
            NEMO_VULKAN_CONTENT MATCHES
            "vk::LayerSettingEXT"
    )

        message(
                STATUS
                "Applying NeMo ggml Vulkan-Headers compatibility patch."
        )

        # ---------------------------------------------------------------------
        # Guard VK_EXT_layer_settings extension advertisement.
        # ---------------------------------------------------------------------

        string(
                REPLACE

                "    if (layer_settings) {\n        extensions.push_back(\"VK_EXT_layer_settings\");\n    }"

                "#ifdef VK_EXT_layer_settings\n    if (layer_settings) {\n        extensions.push_back(\"VK_EXT_layer_settings\");\n    }\n#endif"

                NEMO_VULKAN_CONTENT
                "${NEMO_VULKAN_CONTENT}"
        )

        # ---------------------------------------------------------------------
        # Guard the Vulkan-Hpp LayerSettings structures.
        # ---------------------------------------------------------------------
        #
        # This matches the implementation used by the currently failing
        # NeMo/ggml source.
        # ---------------------------------------------------------------------

        string(
                REPLACE

                "    VkBool32 enable_best_practice = layer_settings;\n    std::vector<vk::LayerSettingEXT> settings = {\n        {\n            \"VK_LAYER_KHRONOS_validation\",\n            \"validate_best_practices\",\n            vk::LayerSettingTypeEXT::eBool32,\n            1,\n            &enable_best_practice\n        },\n    };\n    vk::LayerSettingsCreateInfoEXT layer_setting_info(settings);\n    vk::InstanceCreateInfo instance_create_info(vk::InstanceCreateFlags{}, &app_info, layers, extensions, &layer_setting_info);"

                "#ifdef VK_EXT_layer_settings\n    VkBool32 enable_best_practice = layer_settings;\n    std::vector<vk::LayerSettingEXT> settings = {\n        {\n            \"VK_LAYER_KHRONOS_validation\",\n            \"validate_best_practices\",\n            vk::LayerSettingTypeEXT::eBool32,\n            1,\n            &enable_best_practice\n        },\n    };\n    vk::LayerSettingsCreateInfoEXT layer_setting_info(settings);\n    vk::InstanceCreateInfo instance_create_info(\n        vk::InstanceCreateFlags{},\n        &app_info,\n        layers,\n        extensions,\n        &layer_setting_info\n    );\n#else\n    vk::InstanceCreateInfo instance_create_info(\n        vk::InstanceCreateFlags{},\n        &app_info,\n        layers,\n        extensions\n    );\n#endif"

                NEMO_VULKAN_CONTENT
                "${NEMO_VULKAN_CONTENT}"
        )

        file(
                WRITE
                "${NEMO_VULKAN_CPP}"
                "${NEMO_VULKAN_CONTENT}"
        )

        message(
                STATUS
                "NeMo ggml Vulkan-Headers compatibility patch applied."
        )

    else ()

        message(
                STATUS
                "NeMo ggml Vulkan-Headers compatibility patch not required."
        )

    endif ()

else ()

    message(
            FATAL_ERROR
            "NeMo ggml Vulkan source was not found:\n"
            "${NEMO_VULKAN_CPP}"
    )

endif ()

# -----------------------------------------------------------------------------
# Build NeMo-Speech.cpp
# -----------------------------------------------------------------------------

add_subdirectory(
        "${NEMO_SPEECH_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/nemo-speech"
        EXCLUDE_FROM_ALL
)
if (NOT TARGET nemo_speech_asr)

    message(
            FATAL_ERROR
            "Expected target 'nemo_speech_asr' was not created."
    )

endif ()

if (NOT TARGET nemo_speech_asr_c)

    message(
            FATAL_ERROR
            "Expected target 'nemo_speech_asr_c' was not created."
    )

endif ()

# =============================================================================
# 2. ONNX Runtime
# =============================================================================
#
# Used by openWakeWord.
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
        URL
        "${ONNX_URL}"
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
# 3. Shared QF data directory
# =============================================================================
#
# The model root is <dataRoot>/qf-ml/models, where dataRoot is the
# platform-specific application data directory:
#
#   Linux:   $XDG_DATA_HOME/Questfarer, or $HOME/.local/share/Questfarer
#   macOS:   $HOME/Library/Application Support/Questfarer
#   Windows: %LOCALAPPDATA%/Questfarer
#
# This must match QFPaths::dataRoot() in include/app/QfPaths.h on every
# platform. The two are one contract: if one changes, the other must change
# with it.
# =============================================================================

if (WIN32)

    set(
            QF_DATA_ROOT
            "$ENV{LOCALAPPDATA}/Questfarer/qf-ml"
    )

elseif (APPLE)

    set(
            QF_DATA_ROOT
            "$ENV{HOME}/Library/Application Support/Questfarer/qf-ml"
    )

else ()

    if (
            DEFINED ENV{XDG_DATA_HOME}
            AND NOT "$ENV{XDG_DATA_HOME}" STREQUAL ""
    )

        set(
                QF_DATA_ROOT
                "$ENV{XDG_DATA_HOME}/Questfarer/qf-ml"
        )

    else ()

        set(
                QF_DATA_ROOT
                "$ENV{HOME}/.local/share/Questfarer/qf-ml"
        )

    endif ()

endif ()

set(
        MODELS_DIR
        "${QF_DATA_ROOT}/models"
)

file(
        MAKE_DIRECTORY
        "${MODELS_DIR}"
)

message(
        STATUS
        "QF shared data root: ${QF_DATA_ROOT}"
)

# =============================================================================
# 3A. Hugging Face CLI
# =============================================================================

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
            "Using CMake HTTP fallback."
    )

    set(
            HF_AVAILABLE
            FALSE
    )

endif ()

# =============================================================================
# 3B. Generic downloader
# =============================================================================

function(
        download_model
        url
        dest
        description
)

    if (EXISTS "${dest}")

        message(
                STATUS
                "${description} already exists."
        )

        return()

    endif ()

    get_filename_component(
            destination_dir
            "${dest}"
            DIRECTORY
    )

    file(
            MAKE_DIRECTORY
            "${destination_dir}"
    )

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
                "Failed to download ${description}: "
                "${error_message}\n"
                "URL: ${url}"
        )

    endif ()

    if (NOT EXISTS "${dest}")

        message(
                FATAL_ERROR
                "${description} was not created at ${dest}."
        )

    endif ()

    message(
            STATUS
            "Downloaded ${description}"
    )

endfunction()

# =============================================================================
# 3C. Hugging Face downloader
# =============================================================================

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

        message(
                STATUS
                "${description} already exists."
        )

        return()

    endif ()

    if (HF_AVAILABLE)

        message(
                STATUS
                "Downloading ${description} using Hugging Face..."
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

    message(
            STATUS
            "${description}: downloaded"
    )

endfunction()

# =============================================================================
# 3D. ASR model selection
# =============================================================================
#
# NVIDIA's current model index defines four ASR models:
#
#   nemotron-3.5
#   nemotron-en
#   parakeet-ctc
#   parakeet-tdt
#
# Only the selected model is downloaded.
#
# All selected models are stored in:
#
#   ${MODELS_DIR}/stt
#
# which is shared across applications.
# =============================================================================

set(
        QF_STT_MODEL
        "nemotron-3.5"
        CACHE STRING
        "ASR model: nemotron-3.5, nemotron-en, parakeet-ctc, parakeet-tdt"
)

set_property(
        CACHE
        QF_STT_MODEL
        PROPERTY
        STRINGS
        nemotron-3.5
        nemotron-en
        parakeet-ctc
        parakeet-tdt
)

if (QF_STT_MODEL STREQUAL "nemotron-3.5")

    set(
            QF_STT_REPO
            "nvidia/nemotron-3.5-asr-streaming-0.6b"
    )

    set(
            QF_STT_FILENAME
            "nemotron-3.5-asr-streaming-0.6b.q8_0.gguf"
    )

elseif (QF_STT_MODEL STREQUAL "nemotron-en")

    set(
            QF_STT_REPO
            "nvidia/nemotron-speech-streaming-en-0.6b"
    )

    set(
            QF_STT_FILENAME
            "nemotron-speech-streaming-en-0.6b.q8_0.gguf"
    )

elseif (QF_STT_MODEL STREQUAL "parakeet-ctc")

    set(
            QF_STT_REPO
            "nvidia/parakeet-ctc-1.1b"
    )

    set(
            QF_STT_FILENAME
            "parakeet-ctc-1.1b.q8_0.gguf"
    )

elseif (QF_STT_MODEL STREQUAL "parakeet-tdt")

    set(
            QF_STT_REPO
            "nvidia/parakeet-tdt-0.6b-v3"
    )

    set(
            QF_STT_FILENAME
            "parakeet-tdt-0.6b-v3.q8_0.gguf"
    )

else ()

    message(
            FATAL_ERROR
            "Unknown QF_STT_MODEL='${QF_STT_MODEL}'.\n"
            "Valid values:\n"
            "  nemotron-3.5\n"
            "  nemotron-en\n"
            "  parakeet-ctc\n"
            "  parakeet-tdt"
    )

endif ()

set(
        QF_STT_MODEL_PATH
        "${MODELS_DIR}/stt/${QF_STT_FILENAME}"
)

download_hf_model(
        "${QF_STT_REPO}"
        "${QF_STT_FILENAME}"
        "${QF_STT_MODEL_PATH}"
        "ASR model ${QF_STT_MODEL}"
)

message(
        STATUS
        "Selected ASR model: ${QF_STT_MODEL}"
)

message(
        STATUS
        "ASR model path: ${QF_STT_MODEL_PATH}"
)

# =============================================================================
# 3E. openWakeWord models
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
            "${MODELS_DIR}/wakeword/${MODEL_FILE}"
    )

    download_model(
            "${FILE_URL}"
            "${DEST_PATH}"
            "openWakeWord model ${MODEL_FILE}"
    )

endforeach ()

# =============================================================================
# 4. OCR Libraries
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
# 5. qf-inference
# =============================================================================

add_library(
        qf-inference
        STATIC

        include/inference/LlmClient.h
        src/inference/LlmClient.cpp

        include/inference/InferenceService.h
        src/inference/InferenceService.cpp

        include/inference/LlamaManager.h
        src/inference/LlamaManager.cpp

        include/inference/ModelManager.h
        src/inference/ModelManager.cpp

        include/voice/ITranscriber.h

        include/voice/NemoTranscriber.h
        src/voice/NemoTranscriber.cpp

        include/voice/AudioRecorder.h
        include/voice/WakeWordDetector.h

        include/voice/TtsManager.h
        src/voice/TtsManager.cpp

        include/ui/ModelDialog.h
        src/ui/ModelDialog.cpp

        include/ui/QuantizationGuideDialog.h
        src/ui/QuantizationGuideDialog.cpp

        include/app/QfPaths.h
)

target_include_directories(
        qf-inference

        PUBLIC

        "${CMAKE_CURRENT_SOURCE_DIR}/include"

        PRIVATE

        ${TESSERACT_INCLUDE_DIRS}
        ${LEPTONICA_INCLUDE_DIRS}

        "${NEMO_SPEECH_SOURCE_DIR}/include"

        "${ONNXRUNTIME_INCLUDE_DIR}"
)

target_link_libraries(
        qf-inference

        PUBLIC

        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Multimedia
        Qt6::TextToSpeech

        PRIVATE

        nemo_speech_asr_c

        ONNXRuntime::ONNXRuntime

        ${TESSERACT_LIBRARIES}
        ${LEPTONICA_LIBRARIES}
)

add_dependencies(
        qf-inference
        nemo_speech_asr_c
)

# =============================================================================
# 6. qf-ml-demo
# =============================================================================
#
# Explicitly kept as a normal executable target so CLion can discover it.
# =============================================================================

qt_add_executable(
        qf-ml-demo

        src/main.cpp

        include/app/RuntimeManager.h
        src/app/RuntimeManager.cpp
)

target_link_libraries(
        qf-ml-demo

        PRIVATE

        qf-inference

        Qt6::WebChannel
)

# NeMo-Speech.cpp sets the global runtime/library output directory.
# Reassert the demo output directory explicitly.

set_target_properties(
        qf-ml-demo

        PROPERTIES

        RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/bin"

        RUNTIME_OUTPUT_DIRECTORY_DEBUG
        "${CMAKE_BINARY_DIR}/bin"

        RUNTIME_OUTPUT_DIRECTORY_RELEASE
        "${CMAKE_BINARY_DIR}/bin"

        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO
        "${CMAKE_BINARY_DIR}/bin"

        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL
        "${CMAKE_BINARY_DIR}/bin"
)

# =============================================================================
# 7. Runtime deployment
# =============================================================================

if (WIN32)

    if (EXISTS "${ONNXRUNTIME_DLL}")

        add_custom_command(
                TARGET qf-ml-demo
                POST_BUILD

                COMMAND
                ${CMAKE_COMMAND}
                -E
                copy_if_different
                "${ONNXRUNTIME_DLL}"
                "$<TARGET_FILE_DIR:qf-ml-demo>/onnxruntime.dll"
        )

    endif ()

elseif (APPLE)

    foreach (
            dylib
            ${ONNXRUNTIME_DYLIBS}
    )

        add_custom_command(
                TARGET qf-ml-demo
                POST_BUILD

                COMMAND
                ${CMAKE_COMMAND}
                -E
                copy_if_different
                "${dylib}"
                "$<TARGET_FILE_DIR:qf-ml-demo>/"
        )

    endforeach ()

else ()

    foreach (
            solib
            ${ONNXRUNTIME_SOLIBS}
    )

        add_custom_command(
                TARGET qf-ml-demo
                POST_BUILD

                COMMAND
                ${CMAKE_COMMAND}
                -E
                copy_if_different
                "${solib}"
                "$<TARGET_FILE_DIR:qf-ml-demo>/"
        )

    endforeach ()

endif ()

# -----------------------------------------------------------------------------
# NeMo ASR shared libraries
# -----------------------------------------------------------------------------

if (TARGET nemo_speech_asr_c)

    add_custom_command(
            TARGET qf-ml-demo
            POST_BUILD

            COMMAND
            ${CMAKE_COMMAND}
            -E
            copy_if_different
            "$<TARGET_FILE:nemo_speech_asr_c>"
            "$<TARGET_FILE_DIR:qf-ml-demo>"
    )

endif ()

if (TARGET nemo_speech_asr)

    add_custom_command(
            TARGET qf-ml-demo
            POST_BUILD

            COMMAND
            ${CMAKE_COMMAND}
            -E
            copy_if_different
            "$<TARGET_FILE:nemo_speech_asr>"
            "$<TARGET_FILE_DIR:qf-ml-demo>"
    )

endif ()

# -----------------------------------------------------------------------------
# ggml Vulkan
# -----------------------------------------------------------------------------

if (TARGET ggml-vulkan)

    add_custom_command(
            TARGET qf-ml-demo
            POST_BUILD

            COMMAND
            ${CMAKE_COMMAND}
            -E
            copy_if_different
            "$<TARGET_FILE:ggml-vulkan>"
            "$<TARGET_FILE_DIR:qf-ml-demo>"
    )

endif ()

# -----------------------------------------------------------------------------
# Other shared NeMo/ggml targets
# -----------------------------------------------------------------------------

foreach (
        NEMO_TARGET
        ggml
        ggml-base
        ggml-cpu
        ggml-vulkan
)

    if (TARGET ${NEMO_TARGET})

        get_target_property(
                NEMO_TARGET_TYPE
                ${NEMO_TARGET}
                TYPE
        )

        if (
                NEMO_TARGET_TYPE STREQUAL "SHARED_LIBRARY"
                OR
                NEMO_TARGET_TYPE STREQUAL "MODULE_LIBRARY"
        )

            add_custom_command(
                    TARGET qf-ml-demo
                    POST_BUILD

                    COMMAND
                    ${CMAKE_COMMAND}
                    -E
                    copy_if_different
                    "$<TARGET_FILE:${NEMO_TARGET}>"
                    "$<TARGET_FILE_DIR:qf-ml-demo>"
            )

        endif ()

    endif ()

endforeach ()

# =============================================================================
# 8. RPATH
# =============================================================================

if (UNIX AND NOT APPLE)

    set_target_properties(
            qf-ml-demo

            PROPERTIES

            BUILD_RPATH
            "$ORIGIN;${onnxruntime_prebuilt_SOURCE_DIR}/lib"

            INSTALL_RPATH
            "$ORIGIN"
    )

elseif (APPLE)

    set_target_properties(
            qf-ml-demo

            PROPERTIES

            BUILD_RPATH
            "@loader_path;${onnxruntime_prebuilt_SOURCE_DIR}/lib"

            INSTALL_RPATH
            "@loader_path"
    )

endif ()

# =============================================================================
# 9. Configuration summary
# =============================================================================

message(
        STATUS
        ""
)

message(
        STATUS
        "============================================================"
)

message(
        STATUS
        "QF-ML configuration"
)

message(
        STATUS
        "============================================================"
)

message(
        STATUS
        "CMake:       ${CMAKE_VERSION}"
)

message(
        STATUS
        "Build type:  ${CMAKE_BUILD_TYPE}"
)

message(
        STATUS
        "ASR runtime: NeMo-Speech.cpp"
)

message(
        STATUS
        "ASR model:   ${QF_STT_MODEL}"
)

message(
        STATUS
        "ASR file:    ${QF_STT_MODEL_PATH}"
)

message(
        STATUS
        "Vulkan:      ${GGML_VULKAN}"
)

message(
        STATUS
        "Demo target: qf-ml-demo"
)

message(
        STATUS
        "============================================================"
)

message(
        STATUS
        ""
)