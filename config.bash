#!/bin/bash

export PKG_CONFIG_PATH=/usr/lib64/pkgconfig/:`pwd`/../lib/pkgconfig/
echo $PKG_CONFIG_PATH
OPENCV_INCLUDES=$(pkg-config --cflags opencv)
OPENCV_LIBS=$(pkg-config --libs --static opencv | sed 's/-L-L/-L/' | sed 's/-l64//' | sed 's/Qt5::Core/Qt5Core/')
OPENSSL_INCLUDES=$(pkg-config --cflags openssl)
OPENSSL_LIBS=$(pkg-config --libs openssl)
PREFIX=`pwd`/../

./configure \
    --prefix="$PREFIX" \
    --extra-cflags="-I../include $OPENCV_INCLUDES $OPENSSL_INCLUDES -I/home/blahgeek/nvidia_video_sdk_6.0.1/Samples/common/inc/ -I/opt/BlackmagicDeckLinkSDK/Linux/include" \
    --extra-libs="$OPENCV_LIBS $OPENSSL_LIBS /opt/qt55/lib/libQt5Core.so ../lib/libdoge.a " \
    --enable-gpl \
    --enable-libx264 \
    --enable-nonfree \
    --enable-nvenc \
    --enable-encoder=libx264,mpeg4 \
    --enable-decklink \
    --enable-indev=v4l2,dshow,decklink \
    --enable-outdev=v4l2,decklink \
    --extra-ldflags="-L/usr/local/cuda/lib64 -L/opt/qt55/lib"

