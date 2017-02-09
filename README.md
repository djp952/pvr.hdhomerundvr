#__pvr.hdhomerundvr__  

Unofficial Kodi HDHomeRun DVR PVR Client   
##[__DOCUMENTATION AND DOWNLOADS__](https://github.com/djp952/pvr.hdhomerundvr/wiki)   
   
Copyright (C)2017 Michael G. Brehm    
[MIT LICENSE](https://opensource.org/licenses/MIT)   
   
[__CURL__](https://curl.haxx.se/) - Copyright (C)1996 - 2017, Daniel Stenberg, daniel@haxx.se, and many contributors   
   
**BUILD ENVIRONMENT**  
* Windows 10 x64 15025   
* Visual Studio 2015 (with Git for Windows)   
* Bash on Ubuntu on Windows 16.04.1 LTS   
   
**CONFIGURE BASH ON UBUNTU ON WINDOWS**   
Open "Bash on Ubuntu on Windows"   
```
sudo apt-get install gcc g++ gcc-multilib g++-multilib
```
   
**BUILD**   
Open "Developer Command Prompt for VS2015"   
```
git clone https://github.com/djp952/build --depth=1
git clone https://github.com/djp952/external-kodi-addon-dev-kit -b Krypton --depth=1
git clone https://github.com/djp952/external-sqlite -b sqlite-3.16.2 --depth=1
git clone https://github.com/djp952/prebuilt-libcurl -b libcurl-7.52.1 --depth=1
git clone https://github.com/djp952/prebuilt-libuuid -b libuuid-1.0.3 --depth=1
git clone https://github.com/djp952/pvr.hdhomerundvr -b Krypton
cd pvr.hdhomerundvr
msbuild msbuild.proj

> out\zuki.pvr.hdhomerundvr-win32-krypton-x.x.x.x.zip (Windows / Win32)
> out\zuki.pvr.hdhomerundvr-i686-krypton-x.x.x.x.zip (Linux / i686)
> out\zuki.pvr.hdhomerundvr-x86_64-krypton-x.x.x.x.zip (Linux / x86_64)
```
