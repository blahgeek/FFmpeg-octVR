#!/bin/bash

# FIXME: Though MSYS2 has pkg-config but OpenCV in Windows doesn't support pkg-config now.

#export PKG_CONFIG_PATH=/usr/lib64/pkgconfig/:`pwd`/../lib/pkgconfig/
#echo $PKG_CONFIG_PATH
#OPENCV_INCLUDES=$(pkg-config --cflags opencv)
#OPENCV_LIBS=$(pkg-config --libs --static opencv | sed 's/-L-L/-L/' | sed 's/-l64//')
#OPENSSL_INCLUDES=$(pkg-config --cflags openssl)
#OPENSSL_LIBS=$(pkg-config --libs openssl)
PREFIX=`pwd`/../

./configure \
    --prefix="$PREFIX" \
    --toolchain=msvc \
    --enable-gpl \
    --enable-nonfree \
    --enable-nvenc \
    --enable-pthreads \
    --enable-indev=dshow,decklink \
    --enable-outdev=decklink \
    --extra-cflags="-I../include -I../nvidia_video_sdk_6.0.1/Samples/common/inc -I../BlackmagicDeckLinkSDK/Win/include" \
    --extra-libs="../lib/x64/pthreadVC2.lib ole32.lib user32.lib ../x64/vc12/staticlib/*.lib ../lib/x64/cuda_all.lib"
#    --extra-libs="../lib/x64/pthreadVC2.lib ../x64/vc12/staticlib/opencv_calib3d310.lib         ../x64/vc12/staticlib/opencv_flann310.lib ../x64/vc12/staticlib/opencv_core310.lib            ../x64/vc12/staticlib/opencv_highgui310.lib ../x64/vc12/staticlib/opencv_cudaarithm310.lib      ../x64/vc12/staticlib/opencv_imgcodecs310.lib ../x64/vc12/staticlib/opencv_cudabgsegm310.lib      ../x64/vc12/staticlib/opencv_imgproc310.lib ../x64/vc12/staticlib/opencv_cudacodec310.lib       ../x64/vc12/staticlib/opencv_ml310.lib ../x64/vc12/staticlib/opencv_cudafeatures2d310.lib  ../x64/vc12/staticlib/opencv_objdetect310.lib ../x64/vc12/staticlib/opencv_cudafilters310.lib     ../x64/vc12/staticlib/opencv_octvr310.lib ../x64/vc12/staticlib/opencv_cudaimgproc310.lib     ../x64/vc12/staticlib/opencv_photo310.lib ../x64/vc12/staticlib/opencv_cudalegacy310.lib      ../x64/vc12/staticlib/opencv_shape310.lib ../x64/vc12/staticlib/opencv_cudaobjdetect310.lib   ../x64/vc12/staticlib/opencv_stitching310.lib ../x64/vc12/staticlib/opencv_cudaoptflow310.lib     ../x64/vc12/staticlib/opencv_superres310.lib ../x64/vc12/staticlib/opencv_cudastereo310.lib      ../x64/vc12/staticlib/opencv_ts310.lib ../x64/vc12/staticlib/opencv_cudawarping310.lib     ../x64/vc12/staticlib/opencv_video310.lib ../x64/vc12/staticlib/opencv_cudev310.lib           ../x64/vc12/staticlib/opencv_videoio310.lib ../x64/vc12/staticlib/opencv_features2d310.lib      ../x64/vc12/staticlib/opencv_videostab310.lib"
#    --extra-ldflags="-L../lib/x64" \
#    --enable-decklink \
#    --extra-cflags="-I../include $OPENCV_INCLUDES $OPENSSL_INCLUDES -I/home/blahgeek/nvidia_video_sdk_6.0.1/Samples/common/inc/ -I/opt/BlackmagicDeckLinkSDK/Linux/include" \
#    --extra-libs="../lib/libdoge.a $OPENCV_LIBS $OPENSSL_LIBS" \
#    --enable-gpl \
#    --enable-libx264 \
#    --enable-nonfree \
#    --enable-nvenc \
#    --enable-encoder=libx264,mpeg4 \
#    --enable-decklink \
#    --enable-indev=v4l2,dshow,decklink \
#    --enable-outdev=v4l2,decklink \
#    --extra-ldflags="-L/usr/local/cuda/lib64"

