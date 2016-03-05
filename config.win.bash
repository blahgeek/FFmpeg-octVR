#!/bin/bash

# FIXME: Though MSYS2 has pkg-config but OpenCV in Windows doesn't support pkg-config now.

PREFIX=`pwd`/../
# In Windows, CUDA_PATH should be set when installing CUDA by default.
CUDA_PATH=${CUDA_PATH//\\/\/}

./configure \
    --prefix="$PREFIX" \
    --toolchain=msvc \
    --enable-gpl \
    --enable-nonfree \
    --enable-nvenc \
    --enable-pthreads \
    --enable-decklink \
    --enable-indev=dshow,decklink \
    --enable-outdev=decklink \
    --extra-ldflags="/LIBPATH:\"C:/Qt/Qt5.5.1/5.5/msvc2013_64/lib\"" \
    --extra-cflags="-I../include -I../nvidia_video_sdk_6.0.1/Samples/common/inc -I../BlackmagicDeckLinkSDK/Win/include" \
    --extra-libs="../lib/x64/pthreadVC2.dll ole32.lib user32.lib Qt5Core.lib ../x64/vc12/bin/*.dll \"${CUDA_PATH}\"/lib/x64/*.lib"
