# ddmafb - Distributed DMA framebuffer driver

Its a linux framebuffer driver for driving multiple displays via an AXI DMA.

## Building

This driver is meant to be built as a buildroot package, see [jlbsp](https://github.com/abc123me/jlbsp) 

## Software setup

The driver is based on `drivers/video/fbdev/simplefb.c` and only supports configuration via dervice tree, see below:

```
# TODO
```

## Programmable logic

Typical multiple display setup:

```
Xilinx AXI DMA
  --> axi_fifo_sequencer
    --> axi_pixel_fifo
      --> tft_ili9341 IP blocks (0 to N)
        --> ILI9341 displays (0 to N)
```

Typical single display setup:

```
Xilinx AXI DMA
  --> axi_pixel_fifo
    --> tft_ili9341
      --> ILI9341 display
```

It uses the following logic and an MM2S AXI DMA:

[Vivado IP](https://github.com/abc123me/VivadoIP)

![Example BD](https://raw.githubusercontent.com/abc123me/ddmafb/refs/heads/master/example_bd.png "Quad display example BD")
