cmake_minimum_required(VERSION 3.22.1)
project("vcam15")

# ─── JNI bridge library (loaded by app) ───────────────────────────────────────
add_library(vcam SHARED
    src/main/cpp/jni/vcam_jni.cpp
)
target_include_directories(vcam PRIVATE src/main/cpp)
target_link_libraries(vcam log android)

# ─── Camera hook library (injected into cameraserver) ─────────────────────────
add_library(vcam_hook SHARED
    src/main/cpp/hook/camera_hook.cpp
)
target_include_directories(vcam_hook PRIVATE src/main/cpp)
target_compile_options(vcam_hook PRIVATE -fvisibility=hidden -O2)
target_link_libraries(vcam_hook log dl android)

# ─── Injector executable ──────────────────────────────────────────────────────
add_executable(vcam_injector
    src/main/cpp/injector/injector.cpp
)
target_include_directories(vcam_injector PRIVATE src/main/cpp)
target_link_libraries(vcam_injector log)

# Copy injector to assets after build
add_custom_command(TARGET vcam_injector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/bin
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:vcam_injector>
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/bin/vcam_injector
    COMMENT "Copying vcam_injector to assets/bin/"
)
