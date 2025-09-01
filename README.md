# pwasio

pwasio provides an ASIO to PipeWire for Wine. ASIO is the most common Windows
low-latency audio driver, so is commonly used in audio workstation programs.

### Building

Do the following to build for 64-bit Wine.

```sh
make
```

### Installing

To install 64bit (substitute with the path to the libs for your wine install)
```sh
cp lib/wine/x86_64-windows/pwasio.dll /usr/lib/wine/x86_64-windows/
cp lib/wine/x86_64-unix/pwasio.dll.so /usr/lib/wine/x86_64-unix/
```

After installation, if using an existing Wine prefix you'll also need to copy
the dummy DLL to the prefix path
```sh
cp lib/wine/x86_64-windows/pwasio.dll /path/to/prefix/drive_c/windows/system32
```

If using Proton, the latter needs to be done regardless of whether the prefix
was created before or after library installation, since at the time of writing
the libraries included by Proton are fixed at build time. Finally, the driver
needs to be registered so it's visible to ASIO enabled applications via

``` sh
WINEPREFIX=/path/to/prefix regsvr32 pwasio.dll
```

### General Information

This project comes solely out of frustration of being unable to run Ableton Live
in my current PipeWire/Proton based Arch Linux setup. This project would not at
all be possible without the skeleton laid out in the original WineASIO project,
stability and user-friendliness it is merely a shadow of what that's capable of.
It works, but that's literally it. Despite being a complete refactor, I consider
pwasio a derivative work of WineASIO and as such all original authors from that
have been kept here.

In terms of features, stability and user-friendliness pwasio is merely a shadow
of what WineASIO currently is. Care has also been taken so that this does not
conflict with a WineASIO installation so that users may try out both and figure
out what suits them better. Currently all configuration happens during
compilation time. There are frequent xruns. The program crashes PipeWire
semi-often if things are connected in the wrong order (whatever that means for
you). Development goals include addressing all of the above.

### Change Log

#### 0.0.1
* 01/09/2025: Initial version (GG)

### Legal

Copyright (C) 2006 Robert Reif  
Portions copyright (C) 2007 Ralf Beck  
Portions copyright (C) 2007 Johnny Petrantoni  
Portions copyright (C) 2007 Stephane Letz  
Portions copyright (C) 2008 William Steidtmann  
Portions copyright (C) 2010 Peter L Jones  
Portions copyright (C) 2010 Torben Hohn  
Portions copyright (C) 2010 Nedko Arnaudov  
Portions copyright (C) 2011 Christian Schoenebeck  
Portions copyright (C) 2013 Joakim Hernberg  
Portions copyright (C) 2020-2023 Filipe Coelho  
Portions copyright (C) 2025 Gabriel Golfetti  

pwasio is licensed under GPL v3+, see LICENSE for more details.  
