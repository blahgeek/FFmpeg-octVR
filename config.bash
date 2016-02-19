#!/bin/bash

export PKG_CONFIG_PATH=/usr/lib64/pkgconfig/:`pwd`/../lib/pkgconfig/
echo $PKG_CONFIG_PATH
OPENCV_INCLUDES=$(pkg-config --cflags opencv)
OPENCV_LIBS=$(pkg-config --libs --static opencv | sed 's/-L-L/-L/' | sed 's/-l64//')
OPENSSL_INCLUDES=$(pkg-config --cflags openssl)
OPENSSL_LIBS=$(pkg-config --libs openssl)
PREFIX=`pwd`/../

./configure \
    --prefix="$PREFIX" \
    --extra-cflags="-I../include $OPENCV_INCLUDES $OPENSSL_INCLUDES -I/home/blahgeek/nvidia_video_sdk_6.0.1/Samples/common/inc/ -I/opt/BlackmagicDeckLinkSDK/Linux/include" \
    --extra-libs="../lib/libdoge.a $OPENCV_LIBS $OPENSSL_LIBS" \
    --enable-gpl \
    --enable-libx264 \
    --enable-nonfree \
    --enable-nvenc \
    --enable-encoder=libx264,mpeg4 \
    --enable-decklink \
    --enable-indev=v4l2,dshow,decklink \
    --enable-outdev=v4l2,decklink \
    --extra-ldflags="-L/usr/local/cuda/lib64"

