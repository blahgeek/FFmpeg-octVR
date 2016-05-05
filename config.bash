#!/bin/bash

export PKG_CONFIG_PATH=/usr/lib64/pkgconfig/:`pwd`/../lib/pkgconfig/
echo $PKG_CONFIG_PATH
OPENCV_INCLUDES=$(pkg-config --cflags opencv)
OPENCV_LIBS=$(pkg-config --libs --static opencv | sed 's/-L-L/-L/' | sed 's/-l64//')
PREFIX=`pwd`/../

./configure \
    --prefix="$PREFIX" \
    --extra-cflags="-I../include $OPENCV_INCLUDES -I../nvidia_video_sdk_6.0.1/Samples/common/inc/ -I../BlackmagicDeckLinkSDK/Linux/include" \
    --extra-libs="$OPENCV_LIBS" \
    --enable-shared \
    --enable-nvenc \
    --enable-encoder=mpeg4 \
    --enable-decklink \
    --enable-indev=v4l2,dshow,decklink \
    --enable-outdev=v4l2,decklink \
    --extra-ldflags="-L/usr/local/cuda/lib64 -L/opt/qt55/lib"

