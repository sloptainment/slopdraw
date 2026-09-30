#!/usr/bin/env python3
# Usage: xdrag.py X1 Y1 X2 Y2  -- press button 1 at (X1,Y1), move in steps to (X2,Y2), release. Uses $DISPLAY via libXtst (ctypes).
import ctypes, sys, time
x11 = ctypes.CDLL("libX11.so.6"); xt = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
d = ctypes.c_void_p(x11.XOpenDisplay(None)); assert d.value, "cannot open display"
def move(x, y): xt.XTestFakeMotionEvent(d, -1, int(x), int(y), 0); x11.XFlush(d); time.sleep(0.03)
def button(down): xt.XTestFakeButtonEvent(d, 1, down, 0); x11.XFlush(d); time.sleep(0.1)
x1, y1, x2, y2 = map(int, sys.argv[1:5])
move(x1, y1); button(True)
for i in range(1, 21): move(x1 + (x2 - x1) * i / 20, y1 + (y2 - y1) * i / 20)
button(False); x11.XCloseDisplay(d)
