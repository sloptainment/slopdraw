#!/usr/bin/env python3
# Usage: xdrag.py X1 Y1 X2 Y2 [KEYS]  -- press button 1 at (X1,Y1), move in steps to (X2,Y2), release; KEYS (e.g. space+Control_L) are held throughout. Uses $DISPLAY via libXtst (ctypes).
import ctypes, sys, time
x11 = ctypes.CDLL("libX11.so.6"); xt = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
d = ctypes.c_void_p(x11.XOpenDisplay(None)); assert d.value, "cannot open display"
def move(x, y): xt.XTestFakeMotionEvent(d, -1, int(x), int(y), 0); x11.XFlush(d); time.sleep(0.03)
def button(down): xt.XTestFakeButtonEvent(d, 1, down, 0); x11.XFlush(d); time.sleep(0.1)
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
def key(n, down): xt.XTestFakeKeyEvent(d, x11.XKeysymToKeycode(d, ctypes.c_ulong(x11.XStringToKeysym(n.encode()))), down, 0); x11.XFlush(d); time.sleep(0.1)
x1, y1, x2, y2 = map(int, sys.argv[1:5])
keys = sys.argv[5].split("+") if len(sys.argv) > 5 else []
move(x1, y1)
for k in keys: key(k, True)
button(True)
for i in range(1, 21): move(x1 + (x2 - x1) * i / 20, y1 + (y2 - y1) * i / 20)
button(False)
for k in reversed(keys): key(k, False)
x11.XCloseDisplay(d)
