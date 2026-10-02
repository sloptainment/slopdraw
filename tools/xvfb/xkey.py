#!/usr/bin/env python3
# Usage: xkey.py KEY  -- press and release one key (an X keysym name such as b or Delete), or a chord such as Control_L+w. Uses $DISPLAY via libXtst (ctypes).
import ctypes, sys, time
x11 = ctypes.CDLL("libX11.so.6"); xt = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
d = ctypes.c_void_p(x11.XOpenDisplay(None)); assert d.value, "cannot open display"
ks = [x11.XKeysymToKeycode(d, ctypes.c_ulong(x11.XStringToKeysym(n.encode()))) for n in sys.argv[1].split("+")]; assert all(ks), "unknown key"
for k in ks: xt.XTestFakeKeyEvent(d, k, True, 0); x11.XFlush(d); time.sleep(0.1)
for k in reversed(ks): xt.XTestFakeKeyEvent(d, k, False, 0); x11.XFlush(d); time.sleep(0.1)
x11.XCloseDisplay(d)
