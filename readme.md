
### 环境变量  
source /opt/pkg/petalinux/settings.sh  

### 更新xsa文件  
petalinux-config --get-hw-description ./vivado_project  

###  配置  
petalinux-config -c u-boot
petalinux-config -c kernel
petalinux-config -c rootfs

### 清除（如果xsa更新了ps侧的配置）  
petalinux-build -x mrproper -f  

### 编译  
petalinux-build  

### 打包  
petalinux-package --boot \
      --bif qspi-all.bif \
      --output images/linux/BOOT-QSPI-ALL.BIN \
      --force

### 下载程序  
program_flash \
    -f ./images/linux/BOOT-QSPI-ALL.BIN \
    -offset 0 \
    -flash_type qspi-x4-single \
    -fsbl ./images/linux/zynq_fsbl.elf \
    -verify

###  测试串口
stty -F /dev/ttyS0 115200 cs8 -cstopb -parenb -crtscts -ixon -ixoff raw -echo
cat /dev/ttyS0
printf 'PL_UART_OK\r\n' > /dev/ttyS0

### 测试hdmi显示  
modetest -M xlnx -s 34:1280x720-60@RG16

### OV2640 V4L2 采集  
```sh
media-ctl -p -d /dev/media0
v4l2-ctl -d /dev/video0 --get-fmt-video
v4l2-ctl -d /dev/video0 --stream-mmap=1 --stream-count=1 --stream-to=/tmp/ov2640-test.rgb16
```

### 共享内存测试  
现在把0x18000000~0x1FFFFFFF的128M空间预留作为共享内存使用
测试共享内存，通知pl写共享内存，从偏移地址0x10*4开始写
devmem 0x18000000 32 0x55aa0010
读共享内存测试写是否正确
devmem 0x18000040