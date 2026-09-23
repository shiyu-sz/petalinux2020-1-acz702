
### 环境变量  
source /opt/pkg/petalinux/settings.sh  

### 更新xsa文件  
petalinux-config --get-hw-description ./vivado_project  

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
