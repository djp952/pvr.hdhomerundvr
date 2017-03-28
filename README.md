# __pvr.hdhomerundvr__  

Unofficial Kodi HDHomeRun DVR PVR Client   
## [__DOCUMENTATION AND DOWNLOADS__](https://github.com/djp952/pvr.hdhomerundvr/wiki)   
   
Copyright (C)2017 Michael G. Brehm    
[MIT LICENSE](https://opensource.org/licenses/MIT)   
   
[__CURL__](https://curl.haxx.se/) - Copyright (C)1996 - 2017, Daniel Stenberg, daniel@haxx.se, and many contributors   
[__ZLIB__](http://www.zlib.net/) - Copyright (C)1995-2017 Jean-loup Gailly and Mark Adler   
[__LIBHDHOMERUN__](https://github.com/Silicondust/libhdhomerun) - Copyright (C)2005-2016 Silicondust USA Inc     
   
**BUILD ENVIRONMENT**  
* Windows 10 x64 15063   
* Visual Studio 2015 (with Git for Windows)   
* Bash on Ubuntu on Windows 16.04.1 LTS   
* Android NDK r12b for Windows 64-bit
   
**CONFIGURE BASH ON UBUNTU ON WINDOWS**   
Open "Bash on Ubuntu on Windows"   
```
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install gcc-4.9 g++-4.9 libc6-dev:i386 libstdc++-4.9-dev:i386 lib32gcc-4.9-dev 
sudo apt-get install gcc-4.9-arm-linux-gnueabihf g++-4.9-arm-linux-gnueabihf gcc-4.9-arm-linux-gnueabi g++-4.9-arm-linux-gnueabi gcc-4.9-aarch64-linux-gnu g++-4.9-aarch64-linux-gnu
```
   
**CONFIGURE ANDROID NDK**   
Download the Android NDK r12b for Windows 64-bit:    
[https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip](https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip)   

* Extract the contents of the .zip file somewhere   
* Set a System Environment Variable named ANDROID_NDK_ROOT that points to the extraction location (android-ndk-r12b)   
* Optionally, ANDROID_NDK_ROOT can also be set on the command line prior to executing msbuild:   
```
...
set ANDROID_NDK_ROOT=D:\android-ndk-r12b
msbuild msbuild.proj
...
```
   
**BUILD**   
Open "Developer Command Prompt for VS2015"   
```
git clone https://github.com/djp952/pvr.hdhomerundvr -b Krypton
cd pvr.hdhomerundvr
git submodule update --init
msbuild msbuild.proj

> out\zuki.pvr.hdhomerundvr-windows-win32-krypton-x.x.x.x.zip (windows-Win32)
> out\zuki.pvr.hdhomerundvr-windows-x64-krypton-x.x.x.x.zip (windows-x64)
> out\zuki.pvr.hdhomerundvr-linux-i686-krypton-x.x.x.x.zip (linux-i686)
> out\zuki.pvr.hdhomerundvr-linux-x86_64-krypton-x.x.x.x.zip (linux-x86_64)
> out\zuki.pvr.hdhomerundvr-linux-armel-krypton-x.x.x.x.zip (linux-armel)
> out\zuki.pvr.hdhomerundvr-linux-armhf-krypton-x.x.x.x.zip (linux-armhf)
> out\zuki.pvr.hdhomerundvr-linux-aarch64-krypton-x.x.x.x.zip (linux-aarch64)
> out\zuki.pvr.hdhomerundvr-android-arm-krypton-x.x.x.x.zip (android-arm)
> out\zuki.pvr.hdhomerundvr-android-aarch64-krypton-x.x.x.x.zip (android-aarch64)
> out\zuki.pvr.hdhomerundvr-android-x86-krypton-x.x.x.x.zip (android-x86)
```
   
**LIBHDHOMERUN LICENSE INFORMATION**   
[https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic](https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic)   
This library statically links with code licensed under the GNU Lesser Public License, v2.1 [(LGPL 2.1)](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html).  As per the terms of that license, the maintainer (djp952) must provide the library in an object (or source) format and allow the user to modify and relink against a different version(s) of the LGPL 2.1 libraries.  To use a different or custom version of libhdhomerun the user may alter the contents of the depends/libhdhomerun source directory prior to building this library.   
