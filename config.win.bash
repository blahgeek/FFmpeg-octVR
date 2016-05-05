#!/bin/bash

# FIXME: Though MSYS2 has pkg-config but OpenCV in Windows doesn't support pkg-config now.

PREFIX=`pwd`/../
# In Windows, CUDA_PATH should be set when installing CUDA by default.
CUDA_PATH=${CUDA_PATH//\\/\/}
cp "${CUDA_PATH}"/lib/x64/*.lib .

./configure \
    --prefix="$PREFIX" \
    --toolchain=msvc \
    --enable-shared \
    --enable-nvenc \
    --enable-pthreads \
    --enable-decklink \
    --enable-indev=dshow,decklink \
    --enable-outdev=decklink \
    --extra-ldflags="/LIBPATH:\"C:/Qt/Qt5.5.1/5.5/msvc2013_64/lib\"" \
    --extra-cflags="-I../include -I../include/pthread -I../nvidia_video_sdk_6.0.1/Samples/common/inc -I../BlackmagicDeckLinkSDK/Win/include" \
    --extra-libs="../lib/x64/pthreadVC2.lib ole32.lib user32.lib Qt5Core.lib ../x64/vc12/lib/*.lib cublas.lib cublas_device.lib cuda.lib cudadevrt.lib cudart.lib cudart_static.lib cufft.lib cufftw.lib curand.lib cusolver.lib cusparse.lib nppc.lib nppi.lib npps.lib nvblas.lib nvcuvid.lib nvrtc.lib OpenCL.lib"
