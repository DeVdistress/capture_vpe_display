# capture_vpe_display
test project for build git:://git.ti.com/glsdk/omapdrmtest.git

## how build libdce

```bash
cd /home/workspace

git clone git://git.omapzoom.org/repo/libdce.git

cd /home/workspace/libdce
```

run 'mc_sitara.desktop' or '/home/workspace/start_scripts/05_02_00_10/start_mc_for_sitara'

```bash
./autogen.sh --host=arm-linux-gnueabihf

make

make install DESTDIR=/home/workspace/ti-processor-sdk-linux-am57xx-evm-05.02.00.10/linux-devkit/sysroots/armv7ahf-neon-linux-gnueabi
```
## how build
- **build capture_vpe_display, v4l2capturedisplay, videnc2test**

###### TODO: the problem with the building viddec3test, it is necessary to compile ffmpeg(avformat.h avcodec.h)
```bash
make all
```
- **build v4l2capturedisplay**
```bash
make v4l2capturedisplay
```
- **build videnc2test**
```bash
make videnc2test
```
- **build viddec3test**

###### TODO: the problem with the building viddec3test, it is necessary to compile ffmpeg(avformat.h avcodec.h)
```bash
make viddec3test
```

## info for reading v4l2 capture video

- [capturing a webcam stream using v4l2](http://jwhsmith.net/2014/12/capturing-a-webcam-stream-using-v4l2/)

- [capture-v4l2](https://jayrambhia.com/blog/capture-v4l2)

- [doc about v4l2 from www.kernel.org](https://www.kernel.org/doc/html/v4.14/media/uapi/v4l/capture.c.html)

## MAIN INFO
- [Processor SDK Linux](http://software-dl.ti.com/processor-sdk-linux/esd/docs/latest/linux/index.html)

- [About Deinterlace](https://www.linuxtv.org/downloads/legacy/video4linux/API/V4L2_API/spec-single/v4l2.html#v4l2-field)

- [Gstreamer pipelines for AM572x](https://developer.ridgerun.com/wiki/index.php?title=Gstreamer_pipelines_for_AM572x)

- [not working plugin for deinterlacer](https://github.com/GStreamer/gst-plugins-good/blob/master/sys/v4l2/gstv4l2src.c)

- [Dual Camera example](http://software-dl.ti.com/processor-sdk-linux/esd/docs/latest/linux/Examples_and_Demos/Application_Demos/Dual_Camera_Demo.html)

- [Multimedia](http://software-dl.ti.com/processor-sdk-linux/esd/docs/latest/linux/Foundational_Components_Multimedia_IVAHD.html?highlight=libdce)

- [IPC](http://software-dl.ti.com/processor-sdk-linux/esd/docs/latest/linux/Foundational_Components_IPC.html?highlight=dce)

- [Software_Developers_Guide](https://processors.wiki.ti.com/index.php/DRA7xx_GLSDK_Software_Developers_Guide)

- [Video Analytics example](http://software-dl.ti.com/processor-sdk-linux/esd/docs/latest/linux/Examples_and_Demos/Application_Demos/Video_Analytics.html)

## trouble with interlaced video
**1.** my topic in texas instruments
 [AM5728: Interlaced video capture issue](https://e2e.ti.com/support/processors/f/791/t/835475)

**2.** my topic in texas instruments
 [AM5728: VIP driver issue](https://e2e.ti.com/support/processors/f/791/t/838687)
 
**3.** runing capturevpedisplay https://git.ti.com/glsdk/omapdrmtest/trees/master
 (work fine with deinterlacer on vpe without wayland/weston)
 vpe works with deinterlacer in mode Alternating Field.
 in order to find out the desired mode for display execute: modetest
 ```bash
 /etc/init.d/weston stop && sleep 1 && capturevpedisplay 704 280 yuyv 704 560 yuyv 1 3 -s 35:800x480
```
**4.** test-v4l2-m2m
 - recorded a file my.yuv using yavta
 ```bash
 yavta -c80 -p -F/home/root/my.yuv --skip 2 -t 1/50 -f YUYV -s 704x280 /dev/video2
 ```
 - i converted my interlaced my.yuv file to my deinterlaced my_out.yuv file
 ```bash
 test-v4l2-m2m /dev/video0 /home/root/my.yuv 704 280 yuyv /home/root/my_out.yuv 704 560 yuyv 1 1 80
 ```
 - play on PC
 ```bash
 ffplay -f rawvideo -pix_fmt yuyv422 -video_size 704x560 -framerate 50 -i my_out.yuv
 ```
**5.** twike of gstreamer plugin for working deinterlacer
 Gstreamer doesn't work because VPE accepts only ALTERNATE mode (each field is presented as separate buffer - alternate top and bottom field), 
 while v4l2src plugin supports INTERLACED mode (both fields is presented in a single buffer). To get rid of jitter,
 you will need to modify the v4l2src plugin to support captured field in ALTERNATE mode as understood by VPE.
 Follow below link to understand different modes for interlace arrangements - [About Deinterlace](https://www.linuxtv.org/downloads/legacy/video4linux/API/V4L2_API/spec-single/v4l2.html#v4l2-field), [not working plugin for deinterlacer gstv4l2src.c](https://github.com/GStreamer/gst-plugins-good/blob/master/sys/v4l2/gstv4l2src.c)