cmake_minimum_required(VERSION 3.20)
project(MutantVideoProducer VERSION 0.9.3 DESCRIPTION "Non-linear video editor" LANGUAGES CXX)
# A release build takes its version from the tag (the workflow sets MVP_VERSION).
set(MVP_VERSION "$ENV{MVP_VERSION}")
if(NOT MVP_VERSION)
    set(MVP_VERSION "${PROJECT_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(MVP_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Executable and runtime output folder")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MVP_RUNTIME_DIR}")

# ---------------------------------------------------------------------------
# Dependencies
#
# Windows: Qt and FFmpeg are picked up from ./deps (see README) unless
# CMAKE_PREFIX_PATH / FFMPEG_ROOT point elsewhere.
# Linux:   system Qt 6 + FFmpeg via find_package / pkg-config.
# ---------------------------------------------------------------------------
set(MVP_DEPS_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Folder holding bundled dependencies")

if(WIN32)
    file(GLOB _mvp_qt_dirs LIST_DIRECTORIES true "${MVP_DEPS_DIR}/Qt/6.*/msvc*_64")
    if(_mvp_qt_dirs)
        list(GET _mvp_qt_dirs 0 _mvp_qt_dir)
        list(APPEND CMAKE_PREFIX_PATH "${_mvp_qt_dir}")
    endif()
    if(NOT FFMPEG_ROOT AND EXISTS "${MVP_DEPS_DIR}/ffmpeg/include")
        set(FFMPEG_ROOT "${MVP_DEPS_DIR}/ffmpeg" CACHE PATH "FFmpeg development root (include/, lib/, bin/)")
    endif()
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui OpenGL Widgets Multimedia Network)

set(_mvp_ff_components avdevice avformat avcodec avfilter avutil swscale swresample)
if(FFMPEG_ROOT)
    add_library(mvp_ffmpeg INTERFACE)
    foreach(_c IN LISTS _mvp_ff_components)
        find_library(FFMPEG_${_c}_LIBRARY NAMES ${_c} lib${_c} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(mvp_ffmpeg INTERFACE "${FFMPEG_${_c}_LIBRARY}")
    endforeach()
    target_include_directories(mvp_ffmpeg SYSTEM INTERFACE "${FFMPEG_ROOT}/include")
    file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavdevice>=60 libavformat>=60 libavcodec>=60 libavfilter>=9 libavutil>=58 libswscale>=7 libswresample>=4)
    add_library(mvp_ffmpeg INTERFACE)
    target_link_libraries(mvp_ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /EHsc) # (warning level: per target, at the end)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # Symbols (.pdb) for every build, release too: crash and hang reports name the
    # functions and lines. They are left out of the zip and the installer.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX,C>:/Zi> $<$<COMPILE_LANGUAGE:CXX,C>:/FS>)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
    # Whole-program optimisation for release builds.
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /O2 /Oi /GL")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /LTCG /OPT:REF /OPT:ICF")
    string(APPEND CMAKE_STATIC_LINKER_FLAGS_RELEASE " /LTCG")
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O2")
endif()

# ---------------------------------------------------------------------------
# Core library: media I/O, project model, compositing, mixing, export.
# No widget dependencies, so tests and tools can link it headless.
# ---------------------------------------------------------------------------
add_library(mvp_core STATIC
    src/core/FFmpeg.h            src/core/FFmpeg.cpp
    src/core/Hardware.h          src/core/Hardware.cpp
    src/core/Time.h              src/core/Time.cpp
    src/core/MediaInfo.h         src/core/MediaInfo.cpp
    src/core/VideoDecoder.h      src/core/VideoDecoder.cpp
    src/core/AudioDecoder.h      src/core/AudioDecoder.cpp
    src/core/FrameConverter.h    src/core/FrameConverter.cpp
    src/core/Project.h           src/core/Project.cpp
    src/core/ProjectCollection.h src/core/ProjectCollection.cpp
    src/core/Story.h             src/core/Story.cpp
    src/core/TimelineOps.h       src/core/TimelineOps.cpp
    src/core/Effects.h           src/core/Effects.cpp
    src/core/KeyframeOps.h       src/core/KeyframeOps.cpp
    src/core/Generators.h        src/core/Generators.cpp
    src/core/GeneratorsAnimated.cpp
    src/core/Graphics.h          src/core/Graphics.cpp
    src/core/MotionGraphics.h    src/core/MotionGraphics.cpp
    src/core/CharacterPreset.cpp
    src/core/Captions.h          src/core/Captions.cpp
    src/core/Voice.h             src/core/Voice.cpp
    src/core/AiRuntime.h         src/core/AiRuntime.cpp
    src/core/Profiles.h          src/core/Profiles.cpp
    src/core/Segmentation.h      src/core/Segmentation.cpp
    src/core/Vst3Host.h          src/core/Vst3Host.cpp
    src/core/YouTube.h           src/core/YouTube.cpp
    src/core/VideoEffects.cpp    src/core/Transitions.cpp
    src/core/TransitionKernels.h src/core/TransitionKernels.cpp
    src/core/EffectHelpers.h     src/core/GlitchEffects.cpp
    src/core/Dither.h            src/core/Dither.cpp
    src/core/GpuGlitchShaders.h
    src/core/ConsoleEngine.h     src/core/ConsoleEngine.cpp
    src/core/LiveCapture.h       src/core/LiveCapture.cpp
    src/core/LiveRecorder.h      src/core/LiveRecorder.cpp
    src/core/LiveAudio.h         src/core/LiveAudio.cpp
    src/core/LiveAudioWasapi.h   src/core/LiveAudioWasapi.cpp
    src/core/LivePadSets.h       src/core/LivePadSets.cpp
    src/core/CubeLut.h           src/core/CubeLut.cpp
    src/core/BeatTracker.h       src/core/BeatTracker.cpp
    src/core/RandomChain.h       src/core/RandomChain.cpp
    src/core/LiveModulation.h    src/core/LiveModulation.cpp
    src/core/MidiInput.h         src/core/MidiInput.cpp
    src/core/AudioSync.h         src/core/AudioSync.cpp
    src/core/Multicam.h          src/core/Multicam.cpp
    src/core/OpticalFlow.h       src/core/OpticalFlow.cpp
    src/core/ColorMatch.h        src/core/ColorMatch.cpp
    src/core/NodeGraph.h         src/core/NodeGraph.cpp
    src/core/BeatMarkers.h       src/core/BeatMarkers.cpp
    src/core/EffectMask.h        src/core/EffectMask.cpp
    src/core/Stabilizer.h        src/core/Stabilizer.cpp
    src/core/Proxy.h             src/core/Proxy.cpp
    src/core/PreviewCache.h      src/core/PreviewCache.cpp
    src/core/Spectrogram.h       src/core/Spectrogram.cpp
    src/core/Scopes.h            src/core/Scopes.cpp
    src/core/VideoShare.h        src/core/VideoShare.cpp
    src/core/MotionTracker.h     src/core/MotionTracker.cpp
    src/core/ConsoleEffects.cpp  src/core/GpuConsoleShaders.h
    src/core/ImageOps.h          src/core/ImageOps.cpp
    src/core/Parallel.h          src/core/Parallel.cpp
    src/core/FilterGraph.h       src/core/FilterGraph.cpp
    src/core/Compositor.h        src/core/Compositor.cpp
    src/core/FrameRenderer.h
    src/core/GpuCompositor.h     src/core/GpuCompositor.cpp
    src/core/AudioMixer.h        src/core/AudioMixer.cpp
    src/core/AudioFinishing.h     src/core/AudioFinishing.cpp
    src/core/YouTubeUpload.h      src/core/YouTubeUpload.cpp
    src/core/SceneDetect.h        src/core/SceneDetect.cpp
    src/core/Cuts.h               src/core/Cuts.cpp
    src/core/Reframe.h            src/core/Reframe.cpp
    src/core/GraphicsWorkflow.h  src/core/GraphicsWorkflow.cpp
    src/core/AudioTempo.h        src/core/AudioTempo.cpp
    src/core/AudioStretch.h      src/core/AudioStretch.cpp
    src/core/Presets.h           src/core/Presets.cpp
    src/core/AudioEffects.h      src/core/AudioEffects.cpp
    src/core/PanCrop.h           src/core/PanCrop.cpp
    src/core/FormatRegistry.h    src/core/FormatRegistry.cpp
    src/core/Exporter.h          src/core/Exporter.cpp
    src/core/ExportPresets.cpp
)
target_include_directories(mvp_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(mvp_core PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL mvp_ffmpeg)
# (the OpenGL paint engine draws a scene's GPU pictures as textures: MotionGraphics.cpp)
target_link_libraries(mvp_core PRIVATE Qt6::OpenGLPrivate)
if(WIN32)
    target_link_libraries(mvp_core PUBLIC dwmapi ole32 winmm xinput d3d11 dxgi) # live capture: window list, desktop audio (WASAPI), MIDI; Spout
endif()

# Speech to text for auto captions: whisper.cpp source in deps/whisper (optional).
if(EXISTS "${MVP_DEPS_DIR}/whisper/CMakeLists.txt")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    foreach(_opt WHISPER_BUILD_TESTS WHISPER_BUILD_EXAMPLES WHISPER_BUILD_SERVER GGML_NATIVE GGML_OPENMP)
        set(${_opt} OFF CACHE BOOL "" FORCE) # (not tuned to the build machine: releases run elsewhere)
    endforeach()
    foreach(_opt GGML_AVX GGML_AVX2 GGML_FMA GGML_F16C)
        set(${_opt} ON CACHE BOOL "" FORCE)
    endforeach()
    # On the GPU through Vulkan when the Vulkan SDK is installed (to build; users need only a driver).
    if(NOT DEFINED ENV{VULKAN_SDK})
        file(GLOB _mvp_vulkan LIST_DIRECTORIES true "C:/VulkanSDK/*")
        if(_mvp_vulkan)
            list(GET _mvp_vulkan -1 _mvp_vulkan)
            set(ENV{VULKAN_SDK} "${_mvp_vulkan}")
        endif()
    endif()
    if(WIN32 AND DEFINED ENV{VULKAN_SDK})
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory("${MVP_DEPS_DIR}/whisper" "${CMAKE_BINARY_DIR}/whisper" EXCLUDE_FROM_ALL)
    target_link_libraries(mvp_core PRIVATE whisper)
    target_compile_definitions(mvp_core PRIVATE MVP_WHISPER)
    if(GGML_VULKAN)
        # vulkan-1.dll comes with the graphics driver: loaded only when Whisper uses it, so
        # Mutant Video Producer still starts without one (Captions.cpp checks for it first).
        target_compile_definitions(mvp_core PRIVATE MVP_WHISPER_VULKAN)
        target_link_options(mvp_core INTERFACE "/DELAYLOAD:vulkan-1.dll")
        target_link_libraries(mvp_core INTERFACE delayimp)
    endif()
endif()

# VST3 audio plugins: the interface headers in deps/vst3/pluginterfaces (optional).
if(WIN32 AND EXISTS "${MVP_DEPS_DIR}/vst3/pluginterfaces/vst/ivstcomponent.h")
    target_include_directories(mvp_core SYSTEM PRIVATE "${MVP_DEPS_DIR}/vst3")
    target_compile_definitions(mvp_core PRIVATE MVP_VST3)
endif()

# Person segmentation (Remove Background): ONNX Runtime in deps/onnxruntime (optional),
# with the model from data/models copied next to the programs.
set(MVP_ORT_DIR "${MVP_DEPS_DIR}/onnxruntime")
if(WIN32 AND EXISTS "${MVP_ORT_DIR}/include/onnxruntime_cxx_api.h")
    target_include_directories(mvp_core SYSTEM PRIVATE "${MVP_ORT_DIR}/include")
    target_link_libraries(mvp_core PRIVATE "${MVP_ORT_DIR}/lib/onnxruntime.lib")
    target_compile_definitions(mvp_core PRIVATE MVP_ONNX)
    set(MVP_ORT_DLLS "${MVP_ORT_DIR}/lib/onnxruntime.dll" "${MVP_ORT_DIR}/lib/onnxruntime_providers_shared.dll")
    if(EXISTS "${MVP_ORT_DIR}/lib/DirectML.dll") # the GPU build (Microsoft.ML.OnnxRuntime.DirectML)
        list(APPEND MVP_ORT_DLLS "${MVP_ORT_DIR}/lib/DirectML.dll")
    endif()
endif()

# ---------------------------------------------------------------------------
# Editor application
# ---------------------------------------------------------------------------
if(WIN32)
    set(MVP_WORKER_SUFFIX ".exe")
else()
    set(MVP_WORKER_SUFFIX "")
endif()
option(MVP_BUILD_GUI "Build the editor application (off = core library + self-test only)" ON)
if(MVP_BUILD_GUI)
add_executable(mvp WIN32
    src/main.cpp
    src/engine/PlaybackEngine.h  src/engine/PlaybackEngine.cpp
    src/engine/LiveEngine.h      src/engine/LiveEngine.cpp
    src/engine/MediaCache.h      src/engine/MediaCache.cpp
    src/ui/Theme.h               src/ui/Theme.cpp
    src/ui/MainWindow.h          src/ui/MainWindow.cpp
    src/ui/MainWindowCaptions.cpp
    src/ui/MainWindowVoice.cpp
    src/ui/MainWindowSounds.cpp
    src/ui/MainWindowRuntimes.cpp
    src/ui/MainWindowProfiles.cpp
    src/ui/MainWindowCollection.cpp
    src/ui/StoryPanel.h          src/ui/StoryPanel.cpp
    src/ui/SequenceEditor.h      src/ui/SequenceEditor.cpp
    src/ui/MainWindowNarration.cpp
    tests/transport.cpp tests/transport.h
    tests/collection-ui.cpp tests/collection-ui.h
    # The scripted GUI tests (--editor-test, --live-test): members, so they reach the widgets' internals.
    tests/gui/MainWindowTest.cpp tests/gui/MainWindowTest2.cpp tests/gui/TimelineWidgetTest.cpp tests/gui/LiveViewTest.cpp
    src/ui/MainWindowStory.cpp
    src/ui/VoiceJob.h            src/ui/VoiceJob.cpp
    src/ui/VoiceOverDialog.h     src/ui/VoiceOverDialog.cpp
    src/ui/RegisteredTo.h
    src/ui/MainWindowYouTube.cpp
    src/ui/MainWindowSetup.cpp src/ui/MainWindowTools.cpp src/ui/MainWindowCuts.cpp src/ui/MainWindowMulticam.cpp src/ui/MainWindowRender.cpp
    src/ui/RenderQueue.h src/ui/RenderQueue.cpp
    src/ui/DeliverView.h src/ui/DeliverView.cpp
    src/ui/MainWindowHelp.cpp
    src/ui/CrashReport.h         src/ui/CrashReport.cpp
    src/ui/MonkeyTest.h          src/ui/MonkeyTest.cpp
    src/ui/MainWindowStart.cpp
    src/ui/Help.h
    src/ui/Vst3Window.h          src/ui/Vst3Window.cpp
    src/ui/ShortcutsDialog.h     src/ui/ShortcutsDialog.cpp
    src/ui/TimelineWidget.h      src/ui/TimelineWidget.cpp src/ui/TimelineLayout.h
    src/ui/NodeEffectEditor.cpp src/ui/TimelineWidgetMouse.cpp src/ui/TimelineWidgetMenus.cpp src/ui/TimelineWidgetDrop.cpp src/ui/TimelineWidgetPaint.cpp
    src/ui/PreviewWidget.h       src/ui/PreviewWidget.cpp
    src/ui/LiveView.h            src/ui/LiveView.cpp
    src/ui/LiveClipsBrowser.h    src/ui/LiveClipsBrowser.cpp
    src/ui/LivePadPanel.h        src/ui/LivePadPanel.cpp
    src/ui/MediaBin.h            src/ui/MediaBin.cpp
    src/ui/RelinkDialog.h        src/ui/RelinkDialog.cpp
    src/ui/MixerWindow.h         src/ui/MixerWindow.cpp
    src/ui/AudioFinishingWindow.h src/ui/AudioFinishingWindow.cpp
    src/ui/BeatTuner.h           src/ui/BeatTuner.cpp
    src/ui/MidiHub.h             src/ui/MidiHub.cpp
    src/ui/ProxyQueue.h          src/ui/ProxyQueue.cpp
    src/ui/SystemStats.h         src/ui/SystemStats.cpp
    src/ui/ScopesWidget.h        src/ui/ScopesWidget.cpp
    src/ui/SetlistDialog.h       src/ui/SetlistDialog.cpp
    src/ui/StreamDialog.h        src/ui/StreamDialog.cpp
    src/ui/McpServer.h           src/ui/McpServer.cpp
    src/ui/ModulationDialog.h    src/ui/ModulationDialog.cpp
    src/ui/LiveOverlaysDialog.h  src/ui/LiveOverlaysDialog.cpp
    src/ui/AudioLevels.h
    src/ui/EffectsBrowser.h      src/ui/EffectsBrowser.cpp
    src/ui/AudioMeter.h          src/ui/AudioMeter.cpp
    src/ui/EffectEditor.h        src/ui/EffectEditor.cpp
    src/ui/GraphicsEditor.h      src/ui/GraphicsEditor.cpp
    src/ui/FxTarget.h
    src/ui/Knob.h                src/ui/Knob.cpp
    src/ui/ParamDisplay.h        src/ui/ParamDisplay.cpp
    src/ui/DeviceChainView.h     src/ui/DeviceChainView.cpp
    src/ui/KeyframeEditor.h      src/ui/KeyframeEditor.cpp
    src/ui/PanCropView.h         src/ui/PanCropView.cpp
    src/ui/EventPropertiesDialog.h src/ui/EventPropertiesDialog.cpp
    src/ui/RenderStatusDialog.h   src/ui/RenderStatusDialog.cpp
    src/ui/ExportDialog.h        src/ui/ExportDialog.cpp
    src/ui/ProjectSettingsDialog.h src/ui/ProjectSettingsDialog.cpp
)
set_target_properties(mvp PROPERTIES OUTPUT_NAME "MutantVideoProducer")
qt_add_resources(mvp "mvp_icon" PREFIX "/" FILES data/mvp-icon.png)
target_link_libraries(mvp PRIVATE mvp_core Qt6::Widgets Qt6::Multimedia Qt6::Network)
if(WIN32)
    # VS2019's CMake autogen worker can stall with concurrent parsing of this target.
    set_property(TARGET mvp PROPERTY AUTOGEN_PARALLEL 1)
    target_link_libraries(mvp PRIVATE crypt32 dbghelp pdh) # the stream key (DPAPI); crash minidumps; GPU load in the status bar
endif()
target_compile_definitions(mvp PRIVATE MVP_VERSION="${MVP_VERSION}")
if(MVP_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    target_compile_definitions(mvp PRIVATE MVP_FILE_VERSION=${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0)
else()
    target_compile_definitions(mvp PRIVATE MVP_FILE_VERSION=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
endif()
if(WIN32 AND EXISTS "${MVP_DEPS_DIR}/deno/deno.exe")
    add_custom_command(TARGET mvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mvp>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MVP_DEPS_DIR}/deno/deno.exe" "${MVP_DEPS_DIR}/deno/DENO-LICENSE.md" "$<TARGET_FILE_DIR:mvp>/tools"
        COMMENT "Deploying Deno for YouTube JavaScript challenges")
endif()
add_custom_command(TARGET mvp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/runtime" "$<TARGET_FILE_DIR:mvp>/runtime"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/voice" "$<TARGET_FILE_DIR:mvp>/voice"
    COMMENT "Copying the local voice catalog and worker source")
if(WIN32 AND EXISTS "${MVP_DEPS_DIR}/ai-bootstrap/uv.exe")
    add_custom_command(TARGET mvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MVP_DEPS_DIR}/ai-bootstrap/uv.exe" "${MVP_DEPS_DIR}/ai-bootstrap/UV-LICENSE-MIT.txt" "${MVP_DEPS_DIR}/ai-bootstrap/UV-LICENSE-APACHE.txt" "$<TARGET_FILE_DIR:mvp>/runtime")
endif()
set(MVP_VOICE_BUNDLE "${MVP_DEPS_DIR}/voice-worker/mvp-voice-worker")
option(MVP_BUNDLE_AI "Deploy frozen AI engines and offline voice assets" ON)
set(MVP_VOICE_EXE "mvp-voice-worker")
if(NOT EXISTS "${MVP_VOICE_BUNDLE}" AND EXISTS "${MVP_DEPS_DIR}/voice-worker/montage-voice-worker")
    # Compatibility with portable workers built before the product rename.
    set(MVP_VOICE_BUNDLE "${MVP_DEPS_DIR}/voice-worker/montage-voice-worker")
    set(MVP_VOICE_EXE "montage-voice-worker")
endif()
if(MVP_BUNDLE_AI AND EXISTS "${MVP_VOICE_BUNDLE}/${MVP_VOICE_EXE}${MVP_WORKER_SUFFIX}")
    add_custom_command(TARGET mvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DSOURCE=${MVP_VOICE_BUNDLE}" "-DDESTINATION=$<TARGET_FILE_DIR:mvp>/voice/worker" -P "${CMAKE_SOURCE_DIR}/scripts/deploy-runtime.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MVP_VOICE_BUNDLE}/${MVP_VOICE_EXE}${MVP_WORKER_SUFFIX}" "$<TARGET_FILE_DIR:mvp>/voice/worker/mvp-voice-worker${MVP_WORKER_SUFFIX}"
        COMMENT "Deploying the portable local voice worker")
endif()
if(MVP_BUNDLE_AI AND EXISTS "${MVP_DEPS_DIR}/voice-assets")
    add_custom_command(TARGET mvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${MVP_DEPS_DIR}/voice-assets" "$<TARGET_FILE_DIR:mvp>/voice/assets"
        COMMENT "Deploying offline voice assets")
endif()
add_custom_command(TARGET mvp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/rvc" "$<TARGET_FILE_DIR:mvp>/voice/rvc")
set(MVP_RVC_BUNDLE "${MVP_DEPS_DIR}/rvc-worker/mvp-rvc-worker")
set(MVP_RVC_EXE "mvp-rvc-worker")
if(NOT EXISTS "${MVP_RVC_BUNDLE}" AND EXISTS "${MVP_DEPS_DIR}/rvc-worker/montage-rvc-worker")
    # Compatibility with portable workers built before the product rename.
    set(MVP_RVC_BUNDLE "${MVP_DEPS_DIR}/rvc-worker/montage-rvc-worker")
    set(MVP_RVC_EXE "montage-rvc-worker")
endif()
if(MVP_BUNDLE_AI AND EXISTS "${MVP_RVC_BUNDLE}/${MVP_RVC_EXE}${MVP_WORKER_SUFFIX}")
    add_custom_command(TARGET mvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DSOURCE=${MVP_RVC_BUNDLE}" "-DDESTINATION=$<TARGET_FILE_DIR:mvp>/voice/rvc-worker" -P "${CMAKE_SOURCE_DIR}/scripts/deploy-runtime.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MVP_RVC_BUNDLE}/${MVP_RVC_EXE}${MVP_WORKER_SUFFIX}" "$<TARGET_FILE_DIR:mvp>/voice/rvc-worker/mvp-rvc-worker${MVP_WORKER_SUFFIX}"
        COMMENT "Deploying the portable RVC engine")
endif()
if(MVP_BUNDLE_AI AND EXISTS "${MVP_DEPS_DIR}/rvc-assets")
    add_custom_command(TARGET mvp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${MVP_DEPS_DIR}/rvc-assets" "$<TARGET_FILE_DIR:mvp>/voice/assets")
endif()
if(WIN32)
    target_sources(mvp PRIVATE data/mvp.rc) # the program icon
    # Ninja/MSVC resource compilation does not discover dependencies inside .rc files.
    set_property(SOURCE data/mvp.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/mvp.ico")
endif()
endif()

# ---------------------------------------------------------------------------
# Headless self-test: probes/decodes media files and round-trips exports.
# ---------------------------------------------------------------------------
add_executable(mvp_story_test tests/story.cpp)
target_link_libraries(mvp_story_test PRIVATE mvp_core)
if(MVP_BUILD_GUI)
    add_executable(mvp_graphics_test tests/graphics.cpp src/ui/GraphicsEditor.cpp src/ui/GraphicsEditor.h)
    target_link_libraries(mvp_graphics_test PRIVATE mvp_core Qt6::Widgets)
    add_executable(mvp_motion_test tests/motion.cpp src/ui/GraphicsEditor.cpp src/ui/GraphicsEditor.h)
    target_link_libraries(mvp_motion_test PRIVATE mvp_core Qt6::Widgets)
endif()
add_executable(mvp_adversarial_test tests/adversarial.cpp)
target_link_libraries(mvp_adversarial_test PRIVATE mvp_core)
add_executable(mvp_collection_test tests/collection.cpp)
add_executable(mvp_finishing_test tests/finishing.cpp)
add_executable(mvp_nodes_test tests/nodes.cpp)
target_link_libraries(mvp_nodes_test PRIVATE mvp_core)
add_executable(mvp_keyframes_test tests/keyframes.cpp)
target_link_libraries(mvp_keyframes_test PRIVATE mvp_core)
target_link_libraries(mvp_finishing_test PRIVATE mvp_core)
target_link_libraries(mvp_collection_test PRIVATE mvp_core)
add_executable(mvp_brand_test tests/brand.cpp)
add_executable(mvp_runtime_test tests/ai-runtime.cpp src/core/AiRuntime.cpp)
target_link_libraries(mvp_runtime_test PRIVATE Qt6::Core)
target_include_directories(mvp_runtime_test PRIVATE src)
target_link_libraries(mvp_brand_test PRIVATE Qt6::Core)
target_include_directories(mvp_brand_test PRIVATE src)
add_executable(mvp_selftest tests/selftest.cpp tests/hardening.cpp)
target_link_libraries(mvp_selftest PRIVATE mvp_core)
if(MVP_BUILD_GUI)
    add_executable(mvp_voice_test tests/voice.cpp src/ui/VoiceJob.cpp src/ui/VoiceJob.h)
    target_link_libraries(mvp_voice_test PRIVATE mvp_core)
endif()
# The self-test needs the Qt libraries it links even when the editor (and so
# windeployqt) doesn't pull them in, plus the offscreen platform plugin.
if(WIN32)
    add_custom_command(TARGET mvp_selftest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Gui>"
                "$<TARGET_FILE:Qt6::OpenGL>" "$<TARGET_FILE_DIR:mvp_selftest>"
        COMMENT "Copying Qt libraries for the self-test")
endif()
if(WIN32 AND TARGET Qt6::QOffscreenIntegrationPlugin)
    add_custom_command(TARGET mvp_selftest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mvp_selftest>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
                "$<TARGET_FILE_DIR:mvp_selftest>/platforms/"
        COMMENT "Copying Qt offscreen platform plugin")
endif()

# ---------------------------------------------------------------------------
# Tests, run with ctest (media suites read testmedia/: tests/make_test_media.sh).
# Labels: ci = what the Linux workflow runs; smoke = the quick part the Windows
# workflow runs too; local = needs a real GPU or takes minutes; gui = drives the
# Editor (plays sound). Everything but the GUI suites:
#   ctest --test-dir build/Release -LE gui --output-on-failure
# ---------------------------------------------------------------------------
enable_testing()
set(MVP_TEST_MEDIA "${CMAKE_SOURCE_DIR}/testmedia")
set(MVP_TEST_OUT "${CMAKE_BINARY_DIR}/test-output")
function(mvp_test name labels)
    add_test(NAME ${name} COMMAND ${ARGN} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(${name} PROPERTIES LABELS "${labels}" TIMEOUT 1800)
endfunction()
set(_self $<TARGET_FILE:mvp_selftest>)
set(_clip "${MVP_TEST_MEDIA}/h264_aac.mp4")
mvp_test(probe "ci;smoke" ${_self} probe ${_clip})
mvp_test(probe-webm ci ${_self} probe "${MVP_TEST_MEDIA}/vp8_vorbis.webm")
mvp_test(generators "ci;smoke" ${_self} generators "${MVP_TEST_OUT}/gen")
mvp_test(youtube-options "ci;smoke" ${_self} youtube-options)
mvp_test(effects ci ${_self} effects ${_clip} "${MVP_TEST_OUT}/fx")
mvp_test(speed ci ${_self} speed ${_clip} "${MVP_TEST_OUT}/speed")
foreach(_suite editing audiofx envelopes ramps presets)
    mvp_test(${_suite} ci ${_self} ${_suite} "${MVP_TEST_MEDIA}" "${MVP_TEST_OUT}/${_suite}")
endforeach()
mvp_test(random ci ${_self} random)
mvp_test(beats ci ${_self} beats)
mvp_test(docs ci ${_self} docs docs data/examples src)
mvp_test(sfx "ci;smoke" ${_self} sfx data/sfx)
mvp_test(harden ci ${_self} harden "${MVP_TEST_MEDIA}" data/examples "${MVP_TEST_OUT}/harden" 500)
mvp_test(export local ${_self} export ${_clip} "${MVP_TEST_OUT}/export")
foreach(_suite gpu racks tools consoles)
    mvp_test(${_suite} local ${_self} ${_suite} "${MVP_TEST_MEDIA}" "${MVP_TEST_OUT}/${_suite}")
endforeach()
mvp_test(story local $<TARGET_FILE:mvp_story_test> ${_clip})
mvp_test(adversarial local $<TARGET_FILE:mvp_adversarial_test>)
foreach(_suite collection brand finishing keyframes nodes)
    mvp_test(${_suite} "ci;smoke" $<TARGET_FILE:mvp_${_suite}_test>)
endforeach()
if(WIN32)
    mvp_test(runtime "ci;smoke" $<TARGET_FILE:mvp_runtime_test>)
endif()
if(MVP_BUILD_GUI)
    mvp_test(graphics ci $<TARGET_FILE:mvp_graphics_test>)
    mvp_test(motion ci $<TARGET_FILE:mvp_motion_test>)
    mvp_test(editor gui $<TARGET_FILE:mvp> --editor-test "${MVP_TEST_OUT}/editor.txt" ${_clip})
    mvp_test(devices gui $<TARGET_FILE:mvp> --devices-test "${MVP_TEST_OUT}/devices.txt" ${_clip})
    # Random use, dialogs included, on three seeds (a failing seed replays exactly; see tests/monkey.cmake).
    foreach(_seed 1 2 3)
        mvp_test(monkey-${_seed} "gui;local" ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:mvp> -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${MVP_TEST_OUT}
                 -DSEED=${_seed} -DSTEPS=1500 -P ${CMAKE_SOURCE_DIR}/tests/monkey.cmake)
    endforeach()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(_python_tests ci_builds)
    if(WIN32)
        list(APPEND _python_tests ai_packaging voice_worker)
    endif()
    foreach(_suite ${_python_tests})
        mvp_test(python-${_suite} "ci;smoke" ${Python3_EXECUTABLE} -m unittest discover -s tests -p test_${_suite}.py)
    endforeach()
endif()

# The user guide and the tutorials' example projects (Help menu), next to the
# programs; copied on every build, so edited pages show up without a relink.
add_custom_target(mvp_docs ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/runtime" "$<TARGET_FILE_DIR:mvp_selftest>/runtime"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/docs" "$<TARGET_FILE_DIR:mvp_selftest>/docs"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/examples" "$<TARGET_FILE_DIR:mvp_selftest>/examples"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/sfx" "$<TARGET_FILE_DIR:mvp_selftest>/sfx"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/effects" "$<TARGET_FILE_DIR:mvp_selftest>/effects"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/characters" "$<TARGET_FILE_DIR:mvp_selftest>/characters"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/sets" "$<TARGET_FILE_DIR:mvp_selftest>/sets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/props" "$<TARGET_FILE_DIR:mvp_selftest>/props"
    COMMENT "Copying the user guide and example projects")

# ---------------------------------------------------------------------------
# Windows runtime deployment: FFmpeg DLLs + Qt runtime next to the exe.
# ---------------------------------------------------------------------------
if(WIN32)
    if(FFMPEG_RUNTIME_DLLS)
        # (ffmpeg.exe too: yt-dlp joins YouTube's picture and sound with it)
        add_custom_command(TARGET mvp_selftest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} "${FFMPEG_ROOT}/bin/ffmpeg.exe" "$<TARGET_FILE_DIR:mvp_selftest>"
            COMMENT "Copying FFmpeg runtime")
    endif()
    add_custom_command(TARGET mvp_selftest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/models" "$<TARGET_FILE_DIR:mvp_selftest>/models"
        COMMENT "Copying models")
    if(MVP_ORT_DLLS)
        add_custom_command(TARGET mvp_selftest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MVP_ORT_DLLS} "$<TARGET_FILE_DIR:mvp_selftest>"
            COMMENT "Copying ONNX Runtime")
    endif()
    get_target_property(_mvp_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_mvp_qt_bin "${_mvp_qmake}" DIRECTORY)
    find_program(MVP_WINDEPLOYQT windeployqt HINTS "${_mvp_qt_bin}")
    if(MVP_WINDEPLOYQT AND MVP_BUILD_GUI)
        add_custom_command(TARGET mvp POST_BUILD
            COMMAND "${MVP_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                    --no-compiler-runtime "$<TARGET_FILE:mvp>"
            COMMENT "Deploying Qt runtime")
    endif()
endif()

# ---------------------------------------------------------------------------
# Warnings for this project's own programs and libraries (not the bundled deps):
# MSVC /W4; GCC/Clang -Wall -Wextra (set above). MVP_WARNINGS_AS_ERRORS=ON fails the
# build on any of them.
# ---------------------------------------------------------------------------
option(MVP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
get_property(_mvp_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target ${_mvp_targets})
    get_target_property(_type ${_target} TYPE)
    if(NOT _type MATCHES "EXECUTABLE|STATIC_LIBRARY")
        continue()
    endif()
    if(MSVC)
        # Not reported: C4456, a local hiding an outer local (GCC's -Wall -Wextra doesn't either);
        # C5105, raised by the Windows SDK's own winbase.h.
        target_compile_options(${_target} PRIVATE /W4 /wd4456 /wd5105 $<$<BOOL:${MVP_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(${_target} PRIVATE $<$<BOOL:${MVP_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
endforeach()
