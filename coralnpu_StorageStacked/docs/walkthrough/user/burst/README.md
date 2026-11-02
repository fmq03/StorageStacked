# burst：真实设备怎样检查多拍 AXI

对应源码：[config.json](https://github.com/hy2581/coralnpu_StorageStacked/blob/b90c7140d2717ee0a0dbec8308ae3488510b9acf/user/burst/config.json)、
[burst.cpp](https://github.com/hy2581/coralnpu_StorageStacked/blob/b90c7140d2717ee0a0dbec8308ae3488510b9acf/user/burst/src/burst.cpp)、
[Makefile](https://github.com/hy2581/coralnpu_StorageStacked/blob/b90c7140d2717ee0a0dbec8308ae3488510b9acf/user/burst/src/Makefile)。

这是 `program.type="custom"` 的真实 CoralNPU 设备程序。程序从 `0x90000ff0`
写入八个 32 位字，连续 32 字节经过 4 KiB 地址边界。随后从偏移 1 开始每隔三字节
覆盖一个字节，检查原来未选中的字节仍保留，再逐字回读比较。
邻近的 `0x90000fec` 保存哨兵值，检查窄写没有破坏前面的字。
程序把回读字的异或结果写到 `0x90010010` 并再次读回，最后向 mailbox
`0xc0000000` 写成功或失败状态并执行 `wfi`。

`config.json` 的 `program.expect` 给出要独立核对的地址和 32 位结果，
`axi.burst_beats=4` 为默认四拍。每个原生 16 字节请求转换成四拍、每拍四字节，
AWLEN/ARLEN 为 3，末拍才置 LAST。其它合法拍数为 1、2、8、16。
这里的异或是设备程序的计算结果，不是交付文件的摘要校验。

在源码仓库根目录执行：

```bash
./user/run.sh burst
```

完整矩阵由
[test_burst.py](https://github.com/hy2581/coralnpu_StorageStacked/blob/b90c7140d2717ee0a0dbec8308ae3488510b9acf/coralnpu/runtime/test_burst.py)
运行 1、2、4、8、16 拍，检查实际 AXI 事件中的 LEN/SIZE、两组总线 lane、
字节掩码、4 KiB 边界附近地址、反压与重放。16 拍设置在途上限 1 并开启重放。
校验器还应拒绝早到或缺失的 LAST、读数据位错误、错误 burst 长度和缺失写拍。
这些检查依据实际设备、在线内存、返回数据与波形，不用配置拍数代替握手证据。
