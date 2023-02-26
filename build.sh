#! /bin/bash
###
 # @Author: 一个心态不好的人 842134087@qq.com
 # @Date: 2023-02-26 11:53:53
 # @LastEditors: 一个心态不好的人 842134087@qq.com
 # @LastEditTime: 2023-02-26 11:56:57
 # @FilePath: \IMRSim\build.sh
 # @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
### 

sudo make
sudo make install
sudo depmod --quick
sudo modprobe dm-imrsim
if [ "$1" = "loop" ]; then
    dd if=/dev/zero of=/tmp/imrsim1 bs=4096 seek=$(((256*4+64)*1024*1024/4096-1)) count=1
    losetup /dev/loop1 /tmp/imrsim1
    echo "0 `imrsim_util/imr_format.sh -d /dev/loop1` imrsim /dev/loop1 0" | dmsetup create imrsim
else
    echo "0 20971520 imrsim /dev/sda 0" | dmsetup create imrsim
    dd if=/dev/zero of=/dev/mapper/imrsim bs=4096 seek=$((20971520)) count=32*1024 2> /dev/null 1> /dev/null
fi
