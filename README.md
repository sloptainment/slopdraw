> "you can't generate ai art"
> 
> "i can tell anyway"
> 
> ok how about i make my art in a vibe-coded tool
> 
> i'm the one still holding the tablet pen after all
> 
> no?

## yeah

i love spiro pen

i missed the paint tool sai linework tool

<small>yes i'm aware sai 2 is a thing</small>

blender's grease pencil shrink/fatten works but that's a bit overkill unless i actually wanted 3d in my 2d

inkscape's power stroke fits the bill but that means i have to switch between two apps (the other being krita, what i use now)

so

## mypaint implementation

i believe brushes are just simple dabs, handled by libmypaint and rendered by slopdraw

not all settings are supported, so your mypaint brushes may not work properly

`[slp]` indicates processed by slopdraw

`[myp]` = processed by mypaint

```
[slp] opaque
[slp] opaque_multiply
[slp] hardness
[slp] radius_logarithmic
[slp] elliptical_dab_ratio
[slp] elliptical_dab_angle
[myp] color_h/s/v
[myp] change_color_h/l/hsl_s/v/hsv_s
[myp] anti_aliasing
[myp] opaque_linearize
[myp] snap_to_pixel
[myp] dabs_per_actual_radius/basic_radius
[myp] dabs_per_second
[myp] offset_by_random/speed/x/y/angle*
[myp] radius_by_random
[myp] tracking_noise
[myp] slow_tracking
[myp] slow_tracking_per_dab
[myp] speed*
[myp] direction_filter
[myp] custom_input*
[myp] stroke_*
[myp] gridmap_*
[myp] pressure_gain_log
[slp] eraser
[slp] smudge
[slp] smudge_length/radius_log/transparency
```