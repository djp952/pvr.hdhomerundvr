# __pvr.hdhomerundvr__  

Unofficial Kodi HDHomeRun DVR PVR Client   
## [__USER DOCUMENTATION AND DOWNLOADS__](https://github.com/djp952/pvr.hdhomerundvr/wiki)   
   
Copyright (C)2017 Michael G. Brehm    
[MIT LICENSE](https://opensource.org/licenses/MIT)   
   
[__CURL__](https://curl.haxx.se/) - Copyright (C)1996 - 2017, Daniel Stenberg, daniel@haxx.se, and many contributors   
[__ZLIB__](http://www.zlib.net/) - Copyright (C)1995-2017 Jean-loup Gailly and Mark Adler   
[__LIBHDHOMERUN__](https://github.com/Silicondust/libhdhomerun) - Copyright (C)2005-2016 Silicondust USA Inc     
   
**BUILD ENVIRONMENT**  
* Windows 10 x64 15063   
* Visual Studio 2017 (with VC++ 2015.3 v140 toolset and Windows 8.1 SDK)   
* Bash on Ubuntu on Windows 16.04.1 LTS   
* Android NDK r12b for Windows 64-bit   
* Raspberry Pi development tools
* Optional: Android SDK tools r25.2.3 for Windows   
* Optional: Oracle Java SE Runtime Environment 8   
   
**CONFIGURE BASH ON UBUNTU ON WINDOWS**   
Open "Bash on Ubuntu on Windows"   
```
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install gcc-4.9 g++-4.9 libc6-dev:i386 libstdc++-4.9-dev:i386 lib32gcc-4.9-dev 
sudo apt-get install gcc-4.9-arm-linux-gnueabihf g++-4.9-arm-linux-gnueabihf gcc-4.9-arm-linux-gnueabi g++-4.9-arm-linux-gnueabi gcc-4.9-aarch64-linux-gnu g++-4.9-aarch64-linux-gnu
git clone https://github.com/raspberrypi/tools.git raspberrypi --depth=1
```
   
**CONFIGURE ANDROID NDK**   
Download the Android NDK r12b for Windows 64-bit:    
[https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip](https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip)   

* Extract the contents of the .zip file somewhere   
* Set a System Environment Variable named ANDROID_NDK_ROOT that points to the extracted android-ndk-r12b folder
   
**CONFIGURE ANDROID SDK TOOLS (OPTIONAL)**   
_Android SDK tools are only required for Android APK generation; see "BUILD AND GENERATE MODIFIED KODI ANDROID APKS" below_  
   
Download the Android SDK tools r25.2.3 for Windows:   
[https://dl.google.com/android/repository/tools_r25.2.3-windows.zip](https://dl.google.com/android/repository/tools_r25.2.3-windows.zip)   
   
* Extract the contents of the .zip file somewhere   
* Set a System Environment Variable named ANDROID_SDK_ROOT that points to the location the .zip was extracted   
* Install the build-tools-25.0.2 package:   
```
cd /d %ANDROID_SDK_ROOT%\tools
android update sdk --all -u -t build-tools-25.0.2
```
   
**CONFIGURE ORACLE JAVA SE RUNTIME ENVIRONMENT (OPTIONAL)**   
_Oracle Java SE Runtime Environment is only required for Android APK generation; see "BUILD AND GENERATE MODIFIED KODI ANDROID APKS" below_   
   
Download the latest jre-8xxx-windows-x64.tar.gz from Oracle:   
[Java SE Runtime Environment 8 - Downloads](http://www.oracle.com/technetwork/java/javase/downloads/jre8-downloads-2133155.html)   

* Extract the contents of the jre-8xxx-windows-x64.tar.gz file somewhere   
* Set a System Environment Variable named JAVA_HOME that points to the location the tar.gz was extracted   
* Test Java:   
```
%JAVA_HOME%\bin\java -version
```
   
**BUILD AND GENERATE KODI ADDON PACKAGES**   
Open "Developer Command Prompt for VS2017"   
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
> out\zuki.pvr.hdhomerundvr-raspbian-armhf-krypton-x.x.x.x.zip (raspbian-armhf)
```
   
**BUILD AND GENERATE MODIFIED KODI ANDROID APKS**   
Building the modified Kodi Android APKs requires a Java keystore to be specified on the build command line in order to sign the resultant APK files.  For more information about APK signing and how to generate the keystore, please see [Sign Your App](https://developer.android.com/studio/publish/app-signing.html).   
   
Open "Developer Command Prompt for VS2017"   
```
git clone https://github.com/djp952/pvr.hdhomerundvr -b Krypton
cd pvr.hdhomerundvr
git submodule update --init
msbuild msbuild.proj /t:PackageApk /p:Keystore={path_to_keystore};KeystorePassword={keystore_password}

> out\kodi-x.x-zuki.pvr.hdhomerundvr-arm-x.x.x.x.apk (android-arm)
> out\kodi-x.x-zuki.pvr.hdhomerundvr-aarch64-x.x.x.x.apk (android-aarch64)
> out\kodi-x.x-zuki.pvr.hdhomerundvr-x86-x.x.x.x.apk (android-x86)
```
   
**LIBHDHOMERUN LICENSE INFORMATION**   
[https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic](https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic)   
This library statically links with code licensed under the GNU Lesser Public License, v2.1 [(LGPL 2.1)](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html).  As per the terms of that license, the maintainer (djp952) must provide the library in an object (or source) format and allow the user to modify and relink against a different version(s) of the LGPL 2.1 libraries.  To use a different or custom version of libhdhomerun the user may alter the contents of the depends/libhdhomerun source directory prior to building this library.   
