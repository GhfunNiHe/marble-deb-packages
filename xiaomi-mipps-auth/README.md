# marble 电量计验证

在装有 MiPPS 适配内核的 Redmi Note 12 Turbo / POCO F5 上，安装 ARM64 包后即可插线使用。包内自带 marble 原厂电量计验证程序、Android 链接器和所需的共享库，新装 Debian 也无需挂载 Android 分区或设置充电器型号。

```sh
sudo apt install ./xiaomi-mipps-auth_0.21+marble5_arm64.deb
```

电源驱动就绪或插入充电器时，系统按需运行原厂程序。充电头的 MiPPS 认证由内核独立完成。需要查看结果时可运行以下命令。第一项为电池验证，第二项为充电头认证，值为 `1` 表示对应验证通过。

```sh
xm=/sys/class/power_supply/qcom-battmgr-usb/device/xiaomi
sudo cat "$xm/authentic"
cat "$xm/pd_verifed"
```

电量计首次等待 20 秒仍未得到确认时，服务会复查状态，必要时重新启动一次原厂验证。可以用 `journalctl -b -u xiaomi-mipps-fuel-gauge.service` 查看结果。

## 构建安装包

源码已包含原厂验证程序及其运行文件。在 ARM64 Debian 或 Ubuntu 的包源码目录执行：

```sh
dpkg-buildpackage -b -us -uc
```

生成的 `.deb` 位于上一级目录。
