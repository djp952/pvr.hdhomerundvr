# __pvr.hdhomerundvr__  

Unofficial Kodi HDHomeRun DVR PVR Client   
## [__USER DOCUMENTATION AND DOWNLOADS__](https://github.com/djp952/pvr.hdhomerundvr/wiki)   
   
Copyright (C)2016-2022 Michael G. Brehm    
[MIT LICENSE](https://opensource.org/licenses/MIT)   
   
[__CURL__](https://curl.haxx.se/) - Copyright (C)1996-2022, Daniel Stenberg, daniel@haxx.se, and many contributors   
[__HTTP-STATUS-CODES-CPP__](https://github.com/j-ulrich/http-status-codes-cpp) - Copyright (C) Jochen Ulrich   
[__LIBHDHOMERUN__](https://github.com/Silicondust/libhdhomerun) - Copyright (C)2005-2018 Silicondust USA Inc   
[__LIBXML2__](http://xmlsoft.org/) - Copyright (C)1998-2012 Daniel Veillard   
[__RAPIDJSON__](https://rapidjson.org/) - Copyright (C)2015 THL A29 Limited, a Tencent company, and Milo Yip   
[__ZLIB__](https://zlib.net/) - Copyright (C)1995-2017 Jean-loup Gailly and Mark Adler   
   
## BUILD ENVIRONMENT
**REQUIRED COMPONENTS**   
* Windows 10 x64 20H2 (19042) or later   
* Visual Studio 2022 Community Edition or higher with:    
     * Desktop Development with C++
     * Universal Windows Platform Development
     * Windows 10 SDK (10.0.18362.0)   
     * C++ v141 Universal Windows Platform Tools   
       
* [Windows Subsystem for Linux](https://docs.microsoft.com/en-us/windows/wsl/install-win10) (WSL v1 recommended)   
* [WSL Ubuntu 18.04 LTS Distro](https://www.microsoft.com/store/productId/9N9TNGVNDL3Q)   

**OPTIONAL COMPONENTS**   
* Android NDK r20b for Windows 64-bit   
* Oracle Java SE Runtime Environment 8   
* OSXCROSS Cross-Compiler (with Mac OSX 10.11 SDK)   
   
**REQUIRED: CONFIGURE UBUNTU ON WINDOWS**   
* Open "Ubuntu 18.04 LTS"   
```
sudo dpkg --add-architecture i386
sudo add-apt-repository 'deb http://archive.ubuntu.com/ubuntu/ xenial main universe'
sudo apt-get update
sudo apt-get install gcc-4.9 g++-4.9 libc6-dev:i386 libstdc++-4.9-dev:i386 lib32gcc-4.9-dev
sudo apt-get install gcc-4.9-arm-linux-gnueabihf g++-4.9-arm-linux-gnueabihf gcc-4.9-arm-linux-gnueabi g++-4.9-arm-linux-gnueabi gcc-4.9-aarch64-linux-gnu g++-4.9-aarch64-linux-gnu
```
   
**OPTIONAL: CONFIGURE ANDROID NDK**   
*Necessary to build Android Targets*   
   
Download the Android NDK r20b for Windows 64-bit:    
[https://dl.google.com/android/repository/android-ndk-r20b-windows-x86_64.zip](https://dl.google.com/android/repository/android-ndk-r20b-windows-x86_64.zip)   

* Extract the contents of the .zip file somewhere   
* Set a System Environment Variable named ANDROID_NDK_ROOT that points to the extracted android-ndk-r20b folder
   
**OPTIONAL: CONFIGURE ORACLE JAVA SE RUNTIME ENVIRONMENT AND CREATE PUBLIC-KEY CERTIFICATE**   
*Necessary to build Universal Windows Platform APPX Targets*   
   
Download the latest jre-8xxx-windows-x64.tar.gz from Oracle:   
[Java SE Runtime Environment 8 - Downloads](http://www.oracle.com/technetwork/java/javase/downloads/jre8-downloads-2133155.html)   

* Extract the contents of the jre-8xxx-windows-x64.tar.gz file somewhere   
* Set a System Environment Variable named JAVA_HOME that points to the location the tar.gz was extracted   
* Generate a custom public-key certificate that can be used to sign the generated package(s). Follow the instructions provided by Google at the Android Developer [Sign Your App](https://developer.android.com/studio/publish/app-signing.html) page.   
   
**OPTIONAL: BUILD OSXCROSS CROSS-COMPILER**   
*Necessary to build OS X Targets*   

* Download [Xcode 11.3.1](https://download.developer.apple.com/Developer_Tools/Xcode_11.3.1/Xcode_11.3.1.xip) __(Account required)__ to a location accessible to the WSL Ubuntu 18.04 LTS Distro
* Open "Ubuntu 18.04 LTS"   
```
sudo apt-get install cmake clang llvm-dev liblzma-dev libxml2-dev uuid-dev libssl-dev libbz2-dev zlib1g-dev
cp {Xcode_11.3.1.xip} ~/
git clone https://github.com/tpoechtrager/osxcross --depth=1
osxcross/tools/gen_sdk_package_pbzx.sh ~/Xcode_11.3.1.xip
mv osxcross/MacOSX10.15.sdk.tar.xz osxcross/tarballs/
UNATTENDED=1 osxcross/build.sh
osxcross/build_compiler_rt.sh
sudo mkdir -p /usr/lib/llvm-6.0/lib/clang/6.0.0/include
sudo mkdir -p /usr/lib/llvm-6.0/lib/clang/6.0.0/lib/darwin
sudo cp -rv $(pwd)/osxcross/build/compiler-rt/compiler-rt/include/sanitizer /usr/lib/llvm-6.0/lib/clang/6.0.0/include
sudo cp -v $(pwd)/osxcross/build/compiler-rt/compiler-rt/build/lib/darwin/*.a /usr/lib/llvm-6.0/lib/clang/6.0.0/lib/darwin
sudo cp -v $(pwd)/osxcross/build/compiler-rt/compiler-rt/build/lib/darwin/*.dylib /usr/lib/llvm-6.0/lib/clang/6.0.0/lib/darwin
```
   
## BUILD KODI ADDON PACKAGES
**INITIALIZE SOURCE TREE AND DEPENDENCIES**
* Open "Developer Command Prompt for VS2022"   
```
git clone https://github.com/djp952/pvr.hdhomerundvr -b Nexus
cd pvr.hdhomerundvr
git submodule update --init
```
   
**BUILD ADDON TARGET PACKAGE(S)**   
* Open "Developer Command Prompt for VS2022"   
```
cd pvr.hdhomerundvr
msbuild msbuild.proj [/t:target[;target...]] [/p:parameter=value[;parameter=value...]
```
   
**SUPPORTED TARGET PLATFORMS**   
The MSBUILD script can be used to target one or more platforms by specifying the /t:target parameter.  Targets that produce Android APK packages also require specifying the path to the generated Keystore file (see above) and the keystore password.  The tables below list the available target platforms and the required command line argument(s) to pass to MSBUILD.   In the absence of the /t:target parameter, the default target selection is **windows**.
   
**SETTING DISPLAY VERSION**   
The display version of the addon that will appear in the addon.xml and to the user in Kodi may be overridden by specifying a /p:DisplayVerison={version} argument to any of the build targets.  By default, the display version will be the first three components of the full build number \[MAJOR.MINOR.REVISION\].  For example, to force the display version to the value '2.0.3a', specify /p:DisplayVersion=2.0.3a on the MSBUILD command line.
   
Examples:   
   
> Build just the osx-x86\_64 platform:   
> ```
>msbuild /t:osx-x86_64
> ```
   
> Build all Linux platforms, force display version to '2.0.3a':   
> ```
> msbuild /t:linux /p:DisplayVersion=2.0.3a
> ```
   
*INDIVIDUAL TARGETS*    
   
| Target | Platform | MSBUILD Parameters |
| :-- | :-- | :-- |
| android-aarch64 | Android ARM64 | /t:android-aarch64 |
| android-arm | Android ARM | /t:android-arm |
| android-x86 | Android X86 | /t:android-x86 |
| android-x86\_64 | Android X86\_64 | /t:android-x86\_64 |
| linux-aarch64 | Linux ARM64 | /t:linux-aarch64 |
| linux-armel | Linux ARM | /t:linux-armel |
| linux-armhf | Linux ARM (hard float) | /t:linux-armhf |
| linux-i686 | Linux X86 | /t:linux-i686 |
| linux-x86\_64 | Linux X64 | /t:linux-x86\_64 |
| osx-x86\_64 | Mac OS X X64 | /t:osx-x86\_64 |
| uwp-arm | Universal Windows Platform ARM | /t:uwp-arm |
| uwp-win32 | Universal Windows Platform X86 | /t:uwp-win32 |
| uwp-x64 | Universal Windows Platform X64 | /t:uwp-x64 |
| uwpappx-arm | Universal Windows Platform ARM MSIX | /t:uwpappx-arm /p:Keystore={keystore};KeystorePassword={keystore-password} |
| uwpappx-x64 | Universal Windows Platform X64 MSIX | /t:uwpappx-x64 /p:Keystore={keystore};KeystorePassword={keystore-password} |
| windows-win32 | Windows X86 | /t:windows-win32 |
| windows-x64 | Windows X64 | /t:windows-x64 |
   
*COMBINATION TARGETS*   
   
| Target | Platform(s) | MSBUILD Parameters |
| :-- | :-- | :-- |
| all | All targets | /t:all /p:Keystore={keystore};KeystorePassword={keystore-password} |
| android | All Android targets | /t:android |
| linux | All Linux targets | /t:linux |
| osx | All Mac OS X targets | /t:osx |
| uwp | All Universal Windows Platform targets | /t:uwp |
| uwpappx | All Universal Windows Platform MSIX targets | /t:uwpappx /p:Keystore={keystore};KeystorePassword={keystore-password} |
| windows (default) | All Windows targets | /t:windows |
   
## ADDITIONAL LICENSE INFORMATION
   
**LIBHDHOMERUN**   
[https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic](https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic)   
This library statically links with code licensed under the GNU Lesser Public License, v2.1 [(LGPL 2.1)](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html).  As per the terms of that license, the maintainer (djp952) must provide the library in an object (or source) format and allow the user to modify and relink against a different version(s) of the LGPL 2.1 libraries.  To use a different or custom version of libhdhomerun the user may alter the contents of the depends/libhdhomerun source directory prior to building this library.   
   
**XCODE AND APPLE SDKS AGREEMENT**   
The instructions provided above indirectly reference the use of intellectual material that is the property of Apple, Inc.  This intellectual material is not FOSS (Free and Open Source Software) and by using it you agree to be bound by the terms and conditions set forth by Apple, Inc. in the [Xcode and Apple SDKs Agreement](https://www.apple.com/legal/sla/docs/xcode.pdf).
