#!/bin/bash
# Script used to build Portaudio for multiple Android ABIs, useful for its Oboe host API.
#
# Ensure that ANDROID_NDK environment variable is set to your Android NDK location
# e.g. /Library/Android/sdk/ndk-bundle

if [ -z "$ANDROID_NDK" ]; then
  echo "Please set ANDROID_NDK to the Android NDK folder"
  exit 1
fi

# Build directory
BUILD_DIR=build

CMAKE_ARGS="-H. \
  -DBUILD_SHARED_LIBS=true \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DANDROID_TOOLCHAIN=clang \
  -DANDROID_STL=c++_shared \
  -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX=."

function build_PaOboe {

  ABI=$1
  MINIMUM_API_LEVEL=$2
  ABI_BUILD_DIR=${BUILD_DIR}/${ABI}

  echo "Building Pa_Oboe for ${ABI}"

  mkdir -p ${ABI_BUILD_DIR} ${ABI_BUILD_DIR}/${STAGING_DIR}

  cmake -B${ABI_BUILD_DIR} \
        -DANDROID_ABI=${ABI} \
        -DANDROID_PLATFORM=android-${MINIMUM_API_LEVEL}\
        ${CMAKE_ARGS}

  pushd ${ABI_BUILD_DIR}
  make -j5
  popd
}

#build_PaOboe armeabi-v7a 16
build_PaOboe arm64-v8a 21
#build_PaOboe x86 16
build_PaOboe x86_64 21
