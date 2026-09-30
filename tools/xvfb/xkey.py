#!/usr/bin/env python3
# Usage: xkey.py KEY  -- press and release one key (an X keysym name such as b or Delete). Uses $DISPLAY via libXtst (ctypes).
import ctypes, sys, time
x11 = ctypes.CDLL("libX11.so.6"); xt = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
d = ctypes.c_void_p(x11.XOpenDisplay(None)); assert d.value, "cannot open display"
k = x11.XKeysymToKeycode(d, ctypes.c_ulong(x11.XStringToKeysym(sys.argv[1].encode()))); assert k, "unknown key"
for down in (True, False): xt.XTestFakeKeyEvent(d, k, down, 0); x11.XFlush(d); time.sleep(0.1)
x11.XCloseDisplay(d)
