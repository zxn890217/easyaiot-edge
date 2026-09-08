cmake_minimum_required(VERSION 3.10)

PROJECT(RUNTIME)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -pthread")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")

# Optional: path to official ONNX Runtime C++ SDK (include/ + lib/)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime C++ SDK root")

# Optional: Rockchip NPU (RKNN) inference backend for RK3588 / RK356x / RV11xx.
# Needs the rknpu2 runtime SDK: rknn_api.h + librknnrt.so.
option(RUNTIME_WITH_RKNN "Build the Rockchip RKNN NPU inference backend" OFF)
set(RKNN_SDK_ROOT "" CACHE PATH "rknpu2 SDK root (include/ + lib/)")
find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h
    HINTS ${RKNN_SDK_ROOT}/include ${RKNN_SDK_ROOT}/rknpu2/include
          ${RKNN_SDK_ROOT}/librknn_api/include
    PATHS /usr/include /usr/local/include /usr/include/rknn /usr/local/include/rknn)
# Official rknpu2 releases keep the aarch64 .so under librknn_api/aarch64 (no lib/),
# so accept both layouts; on a running board the driver's /usr/lib copy wins anyway.
find_library(RKNN_RUNTIME_LIBRARY NAMES rknnrt
    HINTS ${RKNN_SDK_ROOT}/lib ${RKNN_SDK_ROOT}/rknpu2/lib
          ${RKNN_SDK_ROOT}/librknn_api/aarch64 ${RKNN_SDK_ROOT}/aarch64
    PATHS /usr/lib /usr/local/lib /usr/lib/aarch64-linux-gnu /usr/local/lib/aarch64-linux-gnu
          /oem/usr/lib /vendor/usr/lib)
if(RUNTIME_WITH_RKNN)
    if(NOT RKNN_INCLUDE_DIR OR NOT RKNN_RUNTIME_LIBRARY)
        message(FATAL_ERROR
            "RUNTIME_WITH_RKNN=ON but rknn_api.h / librknnrt.so were not found. "
            "Pass -DRKNN_SDK_ROOT=<rknpu2 dir> or -DRKNN_INCLUDE_DIR=... -DRKNN_RUNTIME_LIBRARY=...")
    endif()
    message(STATUS "RKNN: include=${RKNN_INCLUDE_DIR} lib=${RKNN_RUNTIME_LIBRARY}")
else()
    message(STATUS "RKNN: backend disabled (RUNTIME_WITH_RKNN=OFF)")
endif()

# Optional: Rockchip MPP codec backend (rkvdec hardware decode + VEPU hardware
# encode) for RK3588 / RK356x. Needs the vendor librockchip_mpp plus its headers:
# neither FFmpeg build on an RK3588 board is configured with --enable-rkmpp, so
# the runtime drives MPP directly instead of going through avcodec.
option(RUNTIME_WITH_MPP "Build the Rockchip rkmpp hardware codec backend" OFF)
set(MPP_SDK_ROOT "" CACHE PATH "MPP SDK root (include/rockchip/ + lib/)")
find_path(MPP_INCLUDE_DIR NAMES rockchip/rk_mpi.h
    HINTS ${MPP_SDK_ROOT}/include ${MPP_SDK_ROOT}
    PATHS /usr/include /usr/local/include)
# Vendor trees also hand over the header directory itself (/usr/include/rockchip);
# <rockchip/rk_mpi.h> then needs its parent on the include path.
if(NOT MPP_INCLUDE_DIR)
    find_path(MPP_ROCKCHIP_HEADER_DIR NAMES rk_mpi.h
        HINTS ${MPP_SDK_ROOT}/include/rockchip ${MPP_SDK_ROOT}/rockchip
        PATHS /usr/include/rockchip /usr/local/include/rockchip)
    if(MPP_ROCKCHIP_HEADER_DIR)
        get_filename_component(MPP_INCLUDE_DIR "${MPP_ROCKCHIP_HEADER_DIR}" DIRECTORY)
    endif()
endif()
find_library(MPP_LIBRARY NAMES rockchip_mpp
    HINTS ${MPP_SDK_ROOT}/lib ${MPP_SDK_ROOT}
    PATHS /usr/lib /usr/local/lib /usr/lib/aarch64-linux-gnu
          /usr/local/lib/aarch64-linux-gnu /oem/usr/lib)
if(NOT MPP_LIBRARY)
    # Board rootfses ship the versioned SONAME only, with no dev symlink. Passing
    # the full path to the linker makes DT_NEEDED take that SONAME, which is what
    # the container sees at runtime.
    find_file(MPP_LIBRARY_FILE NAMES librockchip_mpp.so.0 librockchip_mpp.so.1 librockchip_mpp.so
        HINTS ${MPP_SDK_ROOT}/lib ${MPP_SDK_ROOT}
        PATHS /usr/lib /usr/local/lib /usr/lib/aarch64-linux-gnu
              /usr/local/lib/aarch64-linux-gnu /usr/lib64 /oem/usr/lib)
    if(MPP_LIBRARY_FILE)
        set(MPP_LIBRARY ${MPP_LIBRARY_FILE})
    endif()
endif()
if(RUNTIME_WITH_MPP)
    if(NOT MPP_INCLUDE_DIR OR NOT MPP_LIBRARY)
        message(FATAL_ERROR
            "RUNTIME_WITH_MPP=ON but rockchip/rk_mpi.h / librockchip_mpp were not found. "
            "Run ./install_rk3588.sh mpp-setup on the board, or pass "
            "-DMPP_SDK_ROOT=<dir with include/rockchip + lib/> or -DMPP_INCLUDE_DIR=... -DMPP_LIBRARY=...")
    endif()
    message(STATUS "MPP: include=${MPP_INCLUDE_DIR} lib=${MPP_LIBRARY}")
else()
    message(STATUS "MPP: backend disabled (RUNTIME_WITH_MPP=OFF)")
endif()

find_package(OpenCV REQUIRED)
message(STATUS "OpenCV: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV version: ${OpenCV_VERSION}")

find_package(CURL REQUIRED)

find_library(JSONCPP_LIBRARY NAMES jsoncpp REQUIRED)
# glog >= 0.7 requires the CMake package target so GLOG_USE_GLOG_EXPORT /
# GLOG_EXPORT are propagated; bare find_library() breaks the public headers.
find_package(glog CONFIG QUIET)
if(TARGET glog::glog)
    set(GLOG_LINK_LIBS glog::glog)
    message(STATUS "glog: using CMake target glog::glog")
else()
    find_library(GLOG_LIBRARY NAMES glog REQUIRED)
    set(GLOG_LINK_LIBS ${GLOG_LIBRARY})
    message(STATUS "glog: ${GLOG_LIBRARY} (legacy find_library)")
endif()
find_library(AVCODEC_LIBRARY NAMES avcodec REQUIRED)
find_library(AVFORMAT_LIBRARY NAMES avformat REQUIRED)
find_library(AVUTIL_LIBRARY NAMES avutil REQUIRED)
find_library(SWSCALE_LIBRARY NAMES swscale REQUIRED)
find_library(INIH_LIBRARY NAMES inih)
find_library(PUGIXML_LIBRARY NAMES pugixml)
message(STATUS "jsoncpp: ${JSONCPP_LIBRARY}")
message(STATUS "ffmpeg: ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY}")

include_directories(
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/
    ${CMAKE_SOURCE_DIR}/3rdparty/cpp-httplib/
)

if(ONNXRUNTIME_ROOT)
    include_directories(
        ${ONNXRUNTIME_ROOT}/include
        ${ONNXRUNTIME_ROOT}/include/onnxruntime
        ${ONNXRUNTIME_ROOT}/include/onnxruntime/core/session
    )
    link_directories(${ONNXRUNTIME_ROOT}/lib)
    message(STATUS "ONNXRUNTIME_ROOT: ${ONNXRUNTIME_ROOT}")
else()
    include_directories(
        /usr/include
        /usr/local/include
        /usr/local/include/onnxruntime
    )
endif()

# OpenCV 5 conda layout: include/opencv5
if(DEFINED ENV{CONDA_PREFIX})
    include_directories($ENV{CONDA_PREFIX}/include/opencv5)
    link_directories($ENV{CONDA_PREFIX}/lib)
endif()

file(GLOB_RECURSE Src_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER Src_SOURCES EXCLUDE REGEX ".*YoloEngine_Fixed\\.cpp$")
if(NOT RUNTIME_WITH_RKNN)
    list(FILTER Src_SOURCES EXCLUDE REGEX ".*RknnEngine\\.cpp$")
endif()
if(NOT RUNTIME_WITH_MPP)
    list(FILTER Src_SOURCES EXCLUDE REGEX "/src/mpp/")
endif()

add_executable(${PROJECT_NAME} ${Src_SOURCES})

# Link only modules used by RUNTIME
set(OPENCV_LINK_LIBS
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_dnn
    opencv_geometry
    opencv_highgui
    opencv_videoio
)
message(STATUS "OpenCV link libs (minimal): ${OPENCV_LINK_LIBS}")

set(OPTIONAL_LIBS "")
if(INIH_LIBRARY)
    list(APPEND OPTIONAL_LIBS ${INIH_LIBRARY})
endif()
if(PUGIXML_LIBRARY)
    list(APPEND OPTIONAL_LIBS ${PUGIXML_LIBRARY})
endif()

target_link_libraries(${PROJECT_NAME}
    ${OPENCV_LINK_LIBS}
    ${AVFORMAT_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
    CURL::libcurl
    ${OPTIONAL_LIBS}
    ${JSONCPP_LIBRARY}
    ${GLOG_LINK_LIBS}
    onnxruntime
    pthread
    dl
)

# glog >= 0.7 needs GLOG_USE_GLOG_EXPORT so GLOG_EXPORT is defined; glog 0.6 ignores it.
target_compile_definitions(${PROJECT_NAME} PRIVATE GLOG_USE_GLOG_EXPORT)

if(RUNTIME_WITH_RKNN)
    target_include_directories(${PROJECT_NAME} PRIVATE ${RKNN_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} ${RKNN_RUNTIME_LIBRARY})
    target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_WITH_RKNN=1)
endif()

if(RUNTIME_WITH_MPP)
    target_include_directories(${PROJECT_NAME} PRIVATE ${MPP_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} ${MPP_LIBRARY})
    target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_WITH_MPP=1)
endif()

# Version string injected by install/build scripts (e.g. 0.1.0+gabcdef)
if(NOT DEFINED RUNTIME_VERSION_STR OR RUNTIME_VERSION_STR STREQUAL "")
    set(RUNTIME_VERSION_STR "unknown")
endif()
target_compile_definitions(${PROJECT_NAME} PRIVATE "RUNTIME_VERSION_STR=\"${RUNTIME_VERSION_STR}\"")
message(STATUS "RUNTIME_VERSION_STR: ${RUNTIME_VERSION_STR}")
