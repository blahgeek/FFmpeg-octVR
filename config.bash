#!/bin/bash

export PKG_CONFIG_PATH=`pwd`/../lib/pkgconfig/
OPENCV_INCLUDES=$(pkg-config --cflags opencv)
OPENCV_LIBS=$(pkg-config --libs --static opencv)
PREFIX=`pwd`/../

./configure \
    --prefix="$PREFIX" \
    --extra-cflags="-I../include $OPENCV_INCLUDES" \
    --extra-libs="../lib/libmap.a $OPENCV_LIBS" \
    --extra-ldflags="-L/usr/local/cuda/lib64"

