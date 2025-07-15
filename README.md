# QT5.15.17

## 依赖项

## 1. Debian环境

### 安装qtbase的依赖
```
$ sudo apt build-dep qtbase-opensource-src
```

### 配置准备
```
$ cd qt-everywhere.5.15.17

$ ./configure -prefix /opt/Qt.5.15.17 -opensource -confirm-license -nomake tests
or
$ mkdir & cd build
$ ../configure -prefix /opt/Qt.5.15.17 -release -opensource -confirm-license -plugin-sql-mysql -plugin-sql-sqlite -no-sql-sqlite2 -plugin-sql-tds -system-sqlite -system-harfbuzz -system-zlib -system-libpng -system-libjpeg -system-doubleconversion -system-pcre -openssl -no-rpath -verbose -optimized-qmake -dbus-linked -no-strip -no-separate-debug-info -qpa wayland -xcb -glib -icu -accessibility -no-directfb -no-use-gold-linker -recheck-all -nomake examples -make-tool make

$ make -j `nproc`
$ make install
```

### 为应用编译准备环境
```
export QT_BUILDDIR=/opt/Qt.5.15.17
export LD_LIBRARY_PATH=$QT_BUILDDIR/lib:$LD_LIBRARY_PATH
export QT_QPA_PLATFORM_PLUGIN_PATH=$QT_BUILDDIR/plugins/platforms
export QT_PLUGIN_PATH=$QT_BUILDDIR/plugin
export PKG_CONFIG_PATH=$QT_BUILDDIR/lib/pkgconfig:$PKG_CONFIG_PATH
export PATH=$QT_BUILDDIR/bin:$PATH
```

## 2. Windows Build with MSVC

查看编译帮助 [ qtbase\config_help.txt ]

```bash
> .\configure.bat -help
```
##
- Supported compiler (Visual Studio 2012 or later, MinGW-builds gcc 4.9 or later)
- Perl version 5.12 or later   [ http://www.activestate.com/activeperl/ ] We recommend Strawberry Perl, available from <http://strawberryperl.com/> 
- Python version 2.7 or later  [ https://www.python.org/ftp/python/2.7.17/python-2.7.17.amd64.msi ] QtWebEngine Need
- OpenSSL 3.0.8


### 编译动态库
```bash
> cmd
> mkdir buildqt & cd buildqt
> "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
> set CL=/MP
> set Path=D:\devtools\Strawberry\perl\bin;D:\devtools\Python.2.7.17;D:\devtools\openssl.3.0.8\bin;D:\workspace\qteverywhere.5.15.17\qtwebengine\tools\nodejs;%Path%
> ..\configure.bat -prefix D:\devtools\Qt.5.15.17 -shared -debug-and-release -opensource -confirm-license  -no-sql-sqlite2 -no-rpath -verbose -no-strip -accessibility -no-directfb -no-use-gold-linker -recheck-all -nomake examples -nomake tests -openssl-linked OPENSSL_INCDIR=D:\devtools\openssl.3.0.8\include OPENSSL_LIBDIR=D:\devtools\openssl.3.0.8\lib -proprietary-codecs -webengine-jumbo-build 0 -webengine-kerberos
> nmake
> nmake install
```

### 编译静态库

- 官方指出不支持qtwebengine静态编译 Static builds are not supported. [https://doc.qt.io/archives/qt-5.15/qtwebengine-platform-notes.html]

```bash
> ..\configure.bat -prefix D:\devtools\Qt.5.15.17\static -static -debug-and-release -opensource -confirm-license  -no-sql-sqlite2 -no-rpath -verbose -no-strip -accessibility -no-directfb -no-use-gold-linker -recheck-all -nomake examples -nomake tests -skip qtwebengine -openssl-linked OPENSSL_INCDIR=D:\devtools\openssl.3.0.8\include OPENSSL_LIBDIR=D:\devtools\openssl.3.0.8\lib
```

### 为应用编译准备环境
```bash
>
set QT_BUILDDIR=D:\devtools\Qt.5.15.17
set LD_LIBRARY_PATH=%QT_BUILDDIR%\lib;%LD_LIBRARY_PATH%
set QT_QPA_PLATFORM_PLUGIN_PATH=%QT_BUILDDIR%\plugins\platforms
set QT_PLUGIN_PATH=%QT_BUILDDIR%\plugin
set PKG_CONFIG_PATH=%QT_BUILDDIR%\lib\pkgconfig;%PKG_CONFIG_PATH%
set Path=%QT_BUILDDIR%\bin;D:\devtools\openssl.3.0.8\bin;%Path%
>
```

## 源码下载 [https://download.qt.io/]

## 官方文档 [https://doc.qt.io/archives/qt-5.15/qtwebengine-index.html]

## 版本列表 [https://doc.qt.io/archives/]

## 合并补丁
```bash
>
> git apply .\cve\CVE-2025-23050-qtconnectivity-5.15.diff --directory=qtconnectivity
>
```