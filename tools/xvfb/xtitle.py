#!/usr/bin/env python3
# Usage: xtitle.py  -- print the title (WM_NAME) of every named top-level window on $DISPLAY, one per line. libX11 via ctypes.
import ctypes
x11 = ctypes.CDLL("libX11.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XDefaultRootWindow.restype = ctypes.c_ulong
d = ctypes.c_void_p(x11.XOpenDisplay(None)); assert d.value, "cannot open display"
root, parent, kids, n = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
x11.XQueryTree(d, ctypes.c_ulong(x11.XDefaultRootWindow(d)), ctypes.byref(root), ctypes.byref(parent), ctypes.byref(kids), ctypes.byref(n))
for i in range(n.value):
    name = ctypes.c_char_p()
    if x11.XFetchName(d, ctypes.c_ulong(kids[i]), ctypes.byref(name)) and name.value: print(name.value.decode())
x11.XCloseDisplay(d)
