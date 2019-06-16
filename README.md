# Compicc README

![](http://www.oyranos.org/images/compicc_logo.svg)

The Compiz ICC colour server, or short compicc, lets you colour manage your
whole desktop at once and in hardware. Play movies, watch images on wide or
narrow gamut displays. Each connected monitor is colour corrected for its
own.

CompICC is written to help the many colour management unaware applications.
It takes them off any decision, what to do around monitor profiles. To do 
so CompICC does its work unquestioned. The result is a more consistent 
desktop.


### Features

CompICC uses Oyranos to configure the ICC profiles for each monitor.
The order of monitor configuration inside Oyranos is, first look in the 
Oyranos DB for a explicit configured ICC profile, if no then scan
the installed ICC profiles for one matching the to be setup device, if not 
generate on the fly from the monitors EDID data block, if not 
try from the Xorg log and as a last means take sRGB, if not 
fail miserably ;-)

* instant desktop colour correction on GPU with compiz
* multi monitor support
* ICC Profile in X support
* X Color Management 0.3 DRAFT1 support
* EDID -> ICC profile on the fly
* 16-bit per channel compositing
* hotplug capable
* wide gamut monitor calibration
* support 30-bit monitors

CompICC is a opt out colour correction mechanism. This means, CompICC does 
its work unquestioned. This implicit style of colour management helps to 
render a consistent desktop. Naive applications can continue to assume they
are drawing in sRGB. sRGB is the standard colour space for the internet. 
There are some applications, which want to do own colour corrections for 
displaying large gamut photos, proofing or monitor calibration. Those need 
to tell CompICC not to colour correct a certain region of their window to 
do advanced colour management of their own. After updating these colour 
management aware applications they will work the same as before. Without 
updating to the X Color Management specification they see all monitors as 
in sRGB, which is not wrong for most of them. Calibration tools need to 
support the X Color Management specification to continue to work as 
expected. The only workaround for them is to disable CompICC during 
calibration and ICC profiling.

### Links
* [Development & Sources](https://github.com/compiz-reloaded/compicc)
* [Download](https://github.com/compiz-reloaded/compicc/releases)
* [Authors](AUTHORS.md)
* [Copyright](COPYING.md) BSD
* [ChangeLog](ChangeLog.md)

### Dependencies
* Compiz 0.8.x
* Oyranos colour management system >= 0.9.6

#### Debian
* locales
* build-essential
* libxml2-dev (if you want xml importing/exporting)

* pkg-config
* x11proto-xext-dev
* libxxf86vm-dev
* libxrandr-dev
* libxinerama-dev
* oyranos-0.9.6 +
* doxygen
* libxfixes-dev

#### Fedora
* compiz-devel
(nouveau might need mesa-dri-drivers-experimental)

#### openSUSE
(nouveau might need http://software.opensuse.org/search?q=Mesa-nouveau3d)

* [Binaries](http://www.oyranos.org/compicc/)

#### Debian/Ubuntu:
(nouveau might need libgl1-mesa-dri-experimental)

### Building

    $ ./configure
    $ make
    $ make install

for a local build use:

    $ ./configure --plugindir=$HOME/.compiz/plugins --icondir=$HOME/.local/share/icons --regdir=$HOME/.compiz/metadata

While that is nice for testing, for a regular installation compiz needs to
see its plugins at starttime, typical registred with system wide ldconfig.

### Install
The plugin is a library und should be in your library path during compiz
start. For tests the following might be enough, after substituting the path:

    $ LD_LIBRARY_PATH=/path/to/my/libs compiz --replace ccp

For normal start I found placing the used path in the system library path 
the most relyable way. After substituting the paths name run as root:

    $ echo /path/to/my/libs > /etc/ld.so.conf.d/my_libs.conf
    $ ldconfig

The Oyranos and other libraries must be made visible to the library loader
in the same way.

### Usage
Use ccsm to switch the plugin on. Its named Colour Management, but might be
translated according to your locale settings. It is located under the workspace category.

Advanced settiging can be activated by setting a root window
Atom called \_ICC\_COLOR\_DISPLAY\_ADVANCED to "1". 
Setting to "0" will nethertheless update the colour transformations in 
CompICC.

#### Example

    $ xprop -root -format _ICC_COLOR_DISPLAY_ADVANCED 8s -set _ICC_COLOR_DISPLAY_ADVANCED  1

The desktop colour server can apply proofing and out of gamut marking
to the whole desktop, except the early colour bind regions, by activating
the \_ICC\_COLOR\_DISPLAY\_ADVANCED atom. This is an very specialised feature. 
Applications can be synchronised by the Oyranos settings.

### Trouble
If the plugin is not visible in ccsm, make shure the icon and 
registration file is visible to compiz' configuration manager.

If its clear that the compicc plugin does not run after switching on or 
crashes, look in the $HOME/.xsession-errors or similiar named log file.

If CompIcc runs, but you are not shure if it works, then
a good idea is to use qcmsevents or xprop to look for the
\_ICC\_COLOR\_DESKTOP atom. It should be present on the root window.

    $ xprop -root | grep _ICC_COLOR_DESKTOP

A false colour profile is good for clear diagnostic:

    $ wget http://www.oyranos.org/wiki/images/c/c0/FakeBRG.icc
    $ mv -v FakeBRG.icc ~/.local/shared/color/icc
    $ oyranos-monitor FakeBRG.icc

As a result the monitor should swap colours.

For a colorimetric verification use ArgyllCMS and a according color measurement device. The ICC profile attached to each monitor can be checked with tools like KolorManager, ICC Examin, oyranos-config-synnefo GUIs or oyranos-monitor on the command line.

To report bugs you can run compiz with debug info on and send the log text:

    $ compiz --replace --debug ccp

### Terms for CompICC Color Correction

* **Oyranos, colord** and **ArgyllCMS** support *ICC configuration in X11*, store configuration in a *persitent DB* and handle the *calibration* part.
* **xcalib** is a *calibration only* tool to write the 2D calibration curves into the graphic card hardware. It is used by Oyranos. (Contrary colord and ArgyllCMS have own calibration code.)
* [Calibration versus Characterisation - ArgyllCMS](https://www.argyllcms.com/doc/calvschar.html)
* **CompICC** detects ICC monitor profiles configured by Oyranos and other settings, e.g. night time white point effect. CompICC uses Oyranos to create from these ICC data *3D LookUp Tables* (LUT). CompICC attaches the 3D LUT's as OpenGL 3D textures + shaders to Compiz *window textures*. The OpenGL shaders + 3Dtexture convert pixel values on the GPU in nearly real time.
* CompICC is a plugin for the **Compiz OpenGL compositor**. They run both on top of the X11 windowing system
* **X11** provides API's to connect to *hardware, OpenGL API* and *drivers*. It is *not a compositor* on it's own and has *no access* to window textures.
* [Colour Correction Concepts for Monitors - Oyranos](https://www.oyranos.org/2011/09/colour-correction-concepts-for-monitors/index.html)
