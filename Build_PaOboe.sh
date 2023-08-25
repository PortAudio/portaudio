#!/bin/bash
# Script to build Portaudio_Oboe for multiple Android ABIs
#
# Ensure that ANDROID_NDK environment variable is set to your Android NDK location
# e.g. /Library/Android/sdk/ndk-bundle

ANDROID_NDK=/home/netresults.wintranet/benfatti/Android/Sdk/ndk/23.1.7779620

if [ -z "$ANDROID_NDK" ]; then
  echo "Please set ANDROID_NDK to the Android NDK folder"
  exit 1
fi

# Build directory
BUILD_DIR=lib

CMAKE_ARGS="-H. \
  -DBUILD_SHARED_LIBS=true \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DANDROID_TOOLCHAIN=clang \
  -DANDROID_STL=c++_shared \
  -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX=."

function build_PaOboe {

  echo "Building Pa_Oboe"

  mkdir -p ${BUILD_DIR} ${BUILD_DIR}/${STAGING_DIR}

  cmake -B${BUILD_DIR} \
        -DANDROID_PLATFORM=android-21\
        ${CMAKE_ARGS}

  pushd ${BUILD_DIR}
  make -j5
  popd
}


build_PaOboe