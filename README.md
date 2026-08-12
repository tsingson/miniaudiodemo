
#  dsp

## mcu
stm32f401RCT6
arm cortex-m4 84mhz 有 fpu单精度, mpu , 256kb闪存, 64kb sram, 一个12位 adc, 两个32位定时器 

上电中, 按 boot0 / 复位锓, 松开复位键, 0.5秒松开 boot0 
断电后, 按 boot0 上电 后 0.5秒松开 boot0

串口连接 pa9 (RX) / pa10 (TX)



## 音频 codec

zephyr 4.4.1 控制 PCM5102A, 引脚是 sck / bck / din / lck / vin / gnd, 及 FLT / demp / xsmt / FMT / a3v3 / agnd / rout / agnd / lrout 引脚, 另有 line out 接口

```
```


