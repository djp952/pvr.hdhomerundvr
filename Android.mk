#-----------------------------------------------------------------------------
# Copyright (c) 2017 Michael G. Brehm
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#-----------------------------------------------------------------------------

LOCAL_PATH := $(call my-dir)

# libcrypto
#
include $(CLEAR_VARS)
LOCAL_MODULE := libcrypto-prebuilt
LOCAL_SRC_FILES := depends/libssl/android-$(TARGET_ARCH_ABI)/lib/libcrypto.a
include $(PREBUILT_STATIC_LIBRARY)

# libcurl
#
include $(CLEAR_VARS)
LOCAL_MODULE := libcurl-prebuilt
LOCAL_SRC_FILES := depends/libcurl/android-$(TARGET_ARCH_ABI)/lib/libcurl.a
include $(PREBUILT_STATIC_LIBRARY)

# libssl
#
include $(CLEAR_VARS)
LOCAL_MODULE := libssl-prebuilt
LOCAL_SRC_FILES := depends/libssl/android-$(TARGET_ARCH_ABI)/lib/libssl.a
include $(PREBUILT_STATIC_LIBRARY)

# libuuid
#
include $(CLEAR_VARS)
LOCAL_MODULE := libuuid-prebuilt
LOCAL_SRC_FILES := depends/libuuid/android-$(TARGET_ARCH_ABI)/lib/libuuid.a
include $(PREBUILT_STATIC_LIBRARY)

# libz
#
include $(CLEAR_VARS)
LOCAL_MODULE := libz-prebuilt
LOCAL_SRC_FILES := depends/libz/android-$(TARGET_ARCH_ABI)/lib/libz.a
include $(PREBUILT_STATIC_LIBRARY)

# libhdhomerundvr
#
include $(CLEAR_VARS)
LOCAL_MODULE := hdhomerundvr

LOCAL_C_INCLUDES += \
	depends/kodi-addon-dev-kit/kodi \
	depends/libcurl/android-$(TARGET_ARCH_ABI)/include/curl \
	depends/libssl/android-$(TARGET_ARCH_ABI)/include \
	depends/libuuid/android-$(TARGET_ARCH_ABI)/include \
	depends/libz/android-$(TARGET_ARCH_ABI)/include \
	depends/libhdhomerun \
	depends/sqlite \
	tmp/version
	
LOCAL_CFLAGS += \
	-DSQLITE_THREADSAFE=2 \
	-DSQLITE_ENABLE_JSON1=1 \
	-DSQLITE_TEMP_STORE=3
	
LOCAL_CPP_FEATURES := \
	exceptions \
	rtti
	
LOCAL_CPPFLAGS += \
	-std=c++14 \
	-Wall \
	-Wno-unknown-pragmas
	
# NOTE: The order is important here: libcurl->libssl->libcrypto->libz
LOCAL_STATIC_LIBRARIES += \
	libuuid-prebuilt \
	libcurl-prebuilt \
	libssl-prebuilt \
	libcrypto-prebuilt \
	libz-prebuilt

LOCAL_LDLIBS += \
	-llog

LOCAL_SRC_FILES := \
	depends/libhdhomerun/hdhomerun_debug.c \
	depends/libhdhomerun/hdhomerun_discover.c \
	depends/libhdhomerun/hdhomerun_os_posix.c \
	depends/libhdhomerun/hdhomerun_pkt.c \
	depends/libhdhomerun/hdhomerun_sock_posix.c \
	depends/sqlite/sqlite3.c \
	src/addoncallbacks.cpp \
	src/database.cpp \
	src/dbextension.cpp \
	src/discover.cpp \
	src/livestream.cpp \
	src/pvr.cpp \
	src/pvrcallbacks.cpp \
	src/scheduler.cpp \
	src/sqlite_exception.cpp
	
include $(BUILD_SHARED_LIBRARY)