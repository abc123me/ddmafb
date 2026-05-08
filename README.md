# ddmafb - Distributed DMA framebuffer driver

Its a linux framebuffer driver for driving multiple displays via an AXI DMA.

## Building

This driver is meant to be built as a buildroot package, see [jlbsp](https://github.com/abc123me/jlbsp) 

## Software setup

The driver is based on `drivers/video/fbdev/simplefb.c` and only supports configuration via dervice tree, see below:

```dts
fragment@2 {
  target = <&amba>;
  __overlay__ {
    axidma: axidma@40400000 {
      compatible = "xlnx,axi-dma-1.00.a";
      reg = <0x40400000 0x10000>;
      xlnx,addrwidth = <0x20>;
      xlnx,datawidth = <0x20>;
      clocks = <&clkc 15>;
      clock-names = "s_axi_lite_aclk";
      interrupt-parent = <&intc>;
      interrupts = <0 30 1>;
      dma-ranges = <0x1FC00000 0x1FC00000 0x400000>;
      dma-names = "axidma0";
      #dma-cells = <1>;
      
      axidma0: axidma0@40400000 {
        compatible = "xlnx,axi-dma-mm2s-channel";
        dma-channels = <0x1>;
        interrupt-parent = <&intc>;
        interrupts = <0 30 1>;
        xlnx,datawidth = <0x20>;
        #dma-cells = <1>;
      };
    };
  };
};

fragment@3 {
  target = <&amba>;
  __overlay__ {
    framebuffer0: ddmafb@0 {
      compatible = "ddma-framebuffer";
      status = "okay";
      framerate = <30>;
      width  = <480>;
      height = <640>;
      stride = <960>;
      format = "r5g6b5";
      dmas = <&axidma 0>;
      dma-names = "axidma0";
    };
  };
};
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
