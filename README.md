# __pvr.hdhomerundvr__  

Unofficial Kodi HDHomeRun DVR PVR Client   
## [__USER DOCUMENTATION AND DOWNLOADS__](https://github.com/djp952/pvr.hdhomerundvr/wiki)   
   
Copyright (C)2018 Michael G. Brehm    
[MIT LICENSE](https://opensource.org/licenses/MIT)   
   
[__CURL__](https://curl.haxx.se/) - Copyright (C)1996 - 2018, Daniel Stenberg, daniel@haxx.se, and many contributors   
[__LIBHDHOMERUN__](https://github.com/Silicondust/libhdhomerun) - Copyright (C)2005-2018 Silicondust USA Inc     
   
## BUILD ENVIRONMENT
**REQUIRED COMPONENTS**   
* Windows 10 x64 1803 (17134) "April 2018 Update"   
* Visual Studio 2017 (with VC 2015.3 v140 toolset for Desktop)   
* Windows Subsystem for Linux   
* [Ubuntu on Windows 16.04.4 LTS](https://www.microsoft.com/store/productId/9NBLGGH4MSV6)   

**OPTIONAL COMPONENTS**   
* Android NDK r12b for Windows 64-bit   
* Android SDK tools r25.2.3 for Windows   
* Oracle Java SE Runtime Environment 8   
* Raspberry Pi Cross-Compiler   
* OSXCROSS Cross-Compiler (with Mac OSX 10.11 SDK)   
   
**REQUIRED: CONFIGURE UBUNTU ON WINDOWS**   
* Open "Ubuntu"   
```
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install gcc-4.9 g++-4.9 libc6-dev:i386 libstdc++-4.9-dev:i386 lib32gcc-4.9-dev 
sudo apt-get install gcc-4.9-arm-linux-gnueabihf g++-4.9-arm-linux-gnueabihf gcc-4.9-arm-linux-gnueabi g++-4.9-arm-linux-gnueabi gcc-4.9-aarch64-linux-gnu g++-4.9-aarch64-linux-gnu
```
   
**OPTIONAL: CONFIGURE ANDROID NDK**   
*Necessary to build Android Targets*   
   
Download the Android NDK r12b for Windows 64-bit:    
[https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip](https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip)   

* Extract the contents of the .zip file somewhere   
* Set a System Environment Variable named ANDROID_NDK_ROOT that points to the extracted android-ndk-r12b folder
   
**OPTIONAL: CONFIGURE ANDROID SDK**   
*Necessary to build Android APK Targets*   
   
Download the Android SDK tools r25.2.3 for Windows:   
[https://dl.google.com/android/repository/tools_r25.2.3-windows.zip](https://dl.google.com/android/repository/tools_r25.2.3-windows.zip)   
   
* Extract the contents of the .zip file somewhere   
* Set a System Environment Variable named ANDROID_SDK_ROOT that points to the location the .zip was extracted   
* Open "Developer Command Prompt for VS2017"     
```
cd /d %ANDROID_SDK_ROOT%\tools
android update sdk --all -u -t build-tools-25.0.2
```
      
**OPTIONAL: CONFIGURE ORACLE JAVA SE RUNTIME ENVIRONMENT AND CREATE PUBLIC-KEY CERTIFICATE**   
*Necessary to build Android APK and Universal Windows Platform APPX Targets*   
   
Download the latest jre-8xxx-windows-x64.tar.gz from Oracle:   
[Java SE Runtime Environment 8 - Downloads](http://www.oracle.com/technetwork/java/javase/downloads/jre8-downloads-2133155.html)   

* Extract the contents of the jre-8xxx-windows-x64.tar.gz file somewhere   
* Set a System Environment Variable named JAVA_HOME that points to the location the tar.gz was extracted   
* Generate a custom public-key certificate that can be used to sign the generated Android APK file. Follow the instructions provided by Google at the Android Developer [Sign Your App](https://developer.android.com/studio/publish/app-signing.html) page.   
   
**OPTIONAL: CONFIGURE RASPBERRY PI CROSS-COMPILER**   
*Necessary to build Raspbian Targets*   
   
* Open "Ubuntu"   
```
git clone https://github.com/raspberrypi/tools.git raspberrypi --depth=1
```
   
**OPTIONAL: CONFIGURE OSXCROSS CROSS-COMPILER**   
*Necessary to build OS X Targets*   

* Generate the MAC OSX 10.11 SDK Package for OSXCROSS by following the instructions provided at [PACKAGING THE SDK](https://github.com/tpoechtrager/osxcross#packaging-the-sdk).  The suggested version of Xcode to use when generating the SDK package is Xcode 7.3.1 (May 3, 2016).
* Open "Ubuntu"   
```
sudo apt-get install make clang zlib1g-dev libmpc-dev libmpfr-dev libgmp-dev
git clone https://github.com/tpoechtrager/osxcross --depth=1
cp {MacOSX10.11.sdk.tar.bz2} osxcross/tarballs/
UNATTENDED=1 osxcross/build.sh
GCC_VERSION=4.9.3 osxcross/build_gcc.sh
```
   
## BUILD KODI ADDON PACKAGES
**INITIALIZE SOURCE TREE AND DEPENDENCIES**
* Open "Developer Command Prompt for VS2017"   
```
git clone https://github.com/djp952/pvr.hdhomerundvr -b Leia
cd pvr.hdhomerundvr
git submodule update --init
```
   
**BUILD ADDON TARGET PACKAGE(S)**   
* Open "Developer Command Prompt for VS2017"   
```
cd pvr.hdhomerundvr
msbuild msbuild.proj [/t:target[;target...]] [/p:parameter=value[;parameter=value...]
```
   
**SUPPORTED TARGET PLATFORMS**   
The MSBUILD script can be used to target one or more platforms by specifying the /t:target parameter.  Targets that produce Android APK packages also require specifying the path to the generated Keystore file (see above) and the keystore password.  The tables below list the available target platforms and the required command line argument(s) to pass to MSBUILD.   In the absence of the /t:target parameter, the default target selection is **windows**.
   
Examples:   
   
> Build just the osx-x86\_64 platform:   
> ```
>msbuild /t:osx-x86_64
> ```
   
> Build all Linux platforms:   
> ```
> msbuild /t:linux
> ```
   
> Build all Android APK platforms:   
> ```
> msbuild /t:androidapk /p:Keystore={keystore};KeystorePassword={keystore-password}
> ```
   
*INDIVIDUAL TARGETS*    
   
| Target | Platform | MSBUILD Parameters |
| :-- | :-- | :-- |
| android-aarch64 | Android ARM64 | /t:android-aarch64 |
| android-arm | Android ARM | /t:android-arm |
| android-x86 | Android X86 | /t:android-x86 |
| androidapk-aarch64 | Android ARM64 APK | /t:androidapk-aarch64 /p:Keystore={keystore};KeystorePassword={keystore-password} |
| androidapk-arm | Android ARM APK | /t:androidapk-arm /p:Keystore={keystore};KeystorePassword={keystore-password} |
| linux-aarch64 | Linux ARM64 | /t:linux-aarch64 |
| linux-armel | Linux ARM | /t:linux-armel |
| linux-armhf | Linux ARM (hard float) | /t:linux-armhf |
| linux-i686 | Linux X86 | /t:linux-i686 |
| linux-x86\_64 | Linux X64 | /t:linux-x86\_64 |
| osx-x86\_64 | Mac OS X X64 | /t:osx-x86\_64 |
| raspbian-armhf | Raspbian ARM (hard float) | /t:raspbian-armhf |
| uwp-arm | Universal Windows Platform ARM | /t:uwp-arm |
| uwp-win32 | Universal Windows Platform X86 | /t:uwp-win32 |
| uwp-x64 | Universal Windows Platform X64 | /t:uwp-x64 |
| uwpappx-win32 | Universal Windows Platform X86 APPX | /t:uwpappx-win32 /p:Keystore={keystore};KeystorePassword={keystore-password} |
| uwpappx-x64 | Universal Windows Platform X64 APPX | /t:uwpappx-x64 /p:Keystore={keystore};KeystorePassword={keystore-password} |
| windows-win32 | Windows X86 | /t:windows-win32 |
| windows-x64 | Windows X64 | /t:windows-x64 |
   
*COMBINATION TARGETS*   
   
| Target | Platform(s) | MSBUILD Parameters |
| :-- | :-- | :-- |
| all | All targets | /t:all /p:Keystore={keystore};KeystorePassword={keystore-password} |
| android | All Android targets | /t:android |
| androidapk | All Android APK targets | /t:androidapk /p:Keystore={keystore};KeystorePassword={keystore-password} |
| linux | All Linux targets | /t:linux |
| osx | All Mac OS X targets | /t:osx |
| uwp | All Universal Windows Platform targets | /t:uwp |
| uwpappx | All Universal Windows Platform APPX targets | /t:uwpappx /p:Keystore={keystore};KeystorePassword={keystore-password} |
| windows (default) | All Windows targets | /t:windows |
   
## ADDITIONAL LICENSE INFORMATION
   
**LIBHDHOMERUN**   
[https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic](https://www.gnu.org/licenses/gpl-faq.html#LGPLStaticVsDynamic)   
This library statically links with code licensed under the GNU Lesser Public License, v2.1 [(LGPL 2.1)](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html).  As per the terms of that license, the maintainer (djp952) must provide the library in an object (or source) format and allow the user to modify and relink against a different version(s) of the LGPL 2.1 libraries.  To use a different or custom version of libhdhomerun the user may alter the contents of the depends/libhdhomerun source directory prior to building this library.   
   
**XCODE AND APPLE SDKS AGREEMENT**   
The instructions provided above indirectly reference the use of intellectual material that is the property of Apple, Inc.  This intellectual material is not FOSS (Free and Open Source Software) and by using it you agree to be bound by the terms and conditions set forth by Apple, Inc. in the [Xcode and Apple SDKs Agreement](https://www.apple.com/legal/sla/docs/xcode.pdf).
