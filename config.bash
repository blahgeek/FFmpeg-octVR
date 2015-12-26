#!/bin/bash

export PKG_CONFIG_PATH=`pwd`/../lib/pkgconfig/
OPENCV_INCLUDES=$(pkg-config --cflags opencv)
OPENCV_LIBS=$(pkg-config --libs --static opencv | sed 's/-L-L/-L/' | sed 's/-l64//')
OPENSSL_INCLUDES=$(pkg-config --cflags openssl)
OPENSSL_LIBS=$(pkg-config --libs openssl)
PREFIX=`pwd`/../

./configure \
    --prefix="$PREFIX" \
    --extra-cflags="-I../include $OPENCV_INCLUDES $OPENSSL_INCLUDES" \
    --extra-libs="../lib/libmap.a ../lib/libDL.a $OPENCV_LIBS $OPENSSL_LIBS" \
    --extra-ldflags="-L/usr/local/cuda/lib64"

