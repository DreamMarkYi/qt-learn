cmake_minimum_required(VERSION 3.16)

project(qt-learn VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets )
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets OpenGL OpenGLWidgets)

# ============================================================================
# Live2D Cubism SDK (OpenGL backend)
# ============================================================================
set(CUBISM_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CubismSdk)
set(CORE_PATH      ${CUBISM_ROOT}/Core)
set(FRAMEWORK_PATH ${CUBISM_ROOT}/Framework)
set(GLEW_PATH      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew)
set(STB_PATH       ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)

# --- Cubism Core (prebuilt) -------------------------------------------------
# Core is a C library. The Windows .lib files are MSVC COFF and do not link
# reliably under MinGW, so we link the universal DLL directly. MSVC keeps the
# CRT/compiler-version specific static lib (143 = VS2022, 142 = VS2019).
add_library(Live2DCubismCore INTERFACE)
target_include_directories(Live2DCubismCore INTERFACE ${CORE_PATH}/include)
if(MSVC)
    target_link_libraries(Live2DCubismCore INTERFACE
        ${CORE_PATH}/lib/windows/x86_64/143/Live2DCubismCore_MD.lib)
else()
    target_link_libraries(Live2DCubismCore INTERFACE
        ${CORE_PATH}/dll/windows/x86_64/Live2DCubismCore.dll)
endif()

# --- GLEW (built from source as a static lib) -------------------------------
# Cubism's OpenGL renderer hard-includes <GL/glew.h>; Windows opengl32 only
# exports GL 1.1, so a loader is required. GLEW_STATIC propagates to consumers.
add_library(glew_local STATIC ${GLEW_PATH}/src/glew.c)
target_compile_definitions(glew_local PUBLIC GLEW_STATIC)
target_compile_definitions(glew_local PRIVATE GLEW_NO_GLU)
target_include_directories(glew_local PUBLIC ${GLEW_PATH}/include)
target_link_libraries(glew_local PUBLIC opengl32)
set_target_properties(glew_local PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# --- Cubism Framework (built from source) -----------------------------------
set(FRAMEWORK_SOURCE OpenGL)                 # selects Rendering/OpenGL backend
add_subdirectory(${FRAMEWORK_PATH} ${CMAKE_BINARY_DIR}/CubismFramework)
target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
target_link_libraries(Framework Live2DCubismCore glew_local)
# Framework is plain C++ with no Qt; keep Qt's auto-tools from scanning it.
set_target_properties(Framework PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# MinGW/GCC: SDK casts wglGetProcAddress's PROC return to void* (MSVC-only
# implicit conversion). -fpermissive downgrades that hard error to a warning.
if(NOT MSVC)
    target_compile_options(Framework PRIVATE -fpermissive)
endif()

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(qt-learn
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        widget.hpp
        widget.cpp
        DesktopCapturer.h
        FFmpegStreamer.h
        CameraManager.h
        GlCompositor.h
        GlCompositor.cpp
        Shader/composite.vert
        Shader/composite.frag
        shader.qrc
        Live2DAllocator.h
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET qt-learn APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(qt-learn SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(qt-learn
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(qt-learn PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt6::Multimedia
    Qt6::MultimediaWidgets  Qt6::OpenGL                       # 新增
    Framework                                                 # Live2D (Core + GLEW transitively)
    opengl32   )

# stb_image.h for texture loading in the glue layer.
target_include_directories(qt-learn PRIVATE ${STB_PATH})

# Copy the Cubism Core DLL next to the executable so it can be found at runtime.
add_custom_command(TARGET qt-learn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CORE_PATH}/dll/windows/x86_64/Live2DCubismCore.dll
        $<TARGET_FILE_DIR:qt-learn>)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.qt-learn)
endif()
set_target_properties(qt-learn PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS qt-learn
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ============================================================================
# live2d_viewer —— 独立的 Live2D 模型查看器（自定义 OpenGL 渲染器）
# 不播动画、不接推流，单独开窗把模型静止显示出来。
# 用 Qt 的 QOpenGLFunctions_3_3_Core 自己绘制，不用 Cubism 自带渲染器，
# 因此不需要 GLEW；只用 Framework 的 Model/Math 层（纯 C++）。
# ============================================================================
qt_add_executable(live2d_viewer
    Live2D/main_live2d.cpp
    Live2D/Live2DWidget.h    Live2D/Live2DWidget.cpp
    Live2D/Live2DModel.h     Live2D/Live2DModel.cpp
    Live2D/Live2DRenderer.h  Live2D/Live2DRenderer.cpp
    Live2DAllocator.h
)
target_link_libraries(live2d_viewer PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::OpenGL
    Qt${QT_VERSION_MAJOR}::OpenGLWidgets
    Framework            # Cubism Core + Framework（Model/Math/解析）
    opengl32
)
# Framework 头里的 csmVector2 等通过 CubismModel.hpp 暴露，Core include 已随 Framework 传递。
add_custom_command(TARGET live2d_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CORE_PATH}/dll/windows/x86_64/Live2DCubismCore.dll
        $<TARGET_FILE_DIR:live2d_viewer>)


if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(qt-learn)
endif()
