#!/bin/bash

export PKG_CONFIG_PATH=/usr/lib64/pkgconfig/:`pwd`/../lib/pkgconfig/
echo $PKG_CONFIG_PATH
SODIUM_INCLUDES=$(pkg-config --cflags libsodium)
SODIUM_LIBS=$(pkg-config --libs --static libsodium | sed 's/-L-L/-L/' | sed 's/-l64//')
OPENCV_INCLUDES=$(pkg-config --cflags opencv)
OPENCV_LIBS=$(pkg-config --libs --static opencv | sed 's/-L-L/-L/' | sed 's/-l64//' | sed 's/Qt5::Core/Qt5Core/')
PREFIX=`pwd`/../

./configure \
    --prefix="$PREFIX" \
    --extra-cflags="-DENCRYPT_ARG -DSODIUM_STATIC -I../include $OPENCV_INCLUDES $SODIUM_INCLUDES -I../nvidia_video_sdk_6.0.1/Samples/common/inc/ -I../BlackmagicDeckLinkSDK/Linux/include" \
    --extra-libs="$OPENCV_LIBS $SODIUM_LIBS /opt/qt55/lib/libQt5Core.so" \
    --enable-gpl \
    --enable-nonfree \
    --enable-nvenc \
    --enable-encoder=mpeg4 \
    --enable-decklink \
    --enable-indev=v4l2,dshow,decklink \
    --enable-outdev=v4l2,decklink \
    --extra-ldflags="-L/usr/local/cuda/lib64 -L/opt/qt55/lib"

