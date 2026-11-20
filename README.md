# Virtual Mac on iPad

Virtual Mac on iPad 在兼容的 iPad 上运行 macOS 虚拟机，支持硬件虚拟化和 Metal 图形加速。本项目基于 Virtual Mac on iPad（[nfzerox/VirtualMacOniPad](https://github.com/nfzerox/VirtualMacOniPad) 和 [liukaixiang817/VirtualMacOniPad](https://github.com/liukaixiang817/VirtualMacOniPad)） ，相比上游，本 fork 为 macOS 虚拟机添加了硬件加速编解码，加入了原生触摸板和触摸屏支持，并修复了一些bug。

**当前版本为 `2.2.0beta2`（`1005`）。** [发布说明](VirtualMac/docs/releases/2.2.0beta2-1005.md) · [下载](https://github.com/netsflex/VirtualMacOniPad/releases)

![Virtual Mac on iPad 运行界面](VirtualMac/screenshots/VirtualMac.png)

## 运行环境

- 宿主需要 rootless 越狱的 iPadOS 16.1 至 16.3.1，以及 ElleKit 或 libhooker。暂不支持 roothide 模式的越狱环境。
- 本版已在 11 英寸 iPad Pro (2021) (`iPad13,5`)、iPadOS 16.3.1 和 Dopamine 环境下实测通过，并测试了多个版本的来宾系统，包括 macOS Sequoia 15.6.1、macOS 27.2 和 macOS 27.3 Developer Beta 1 等。

## 安装与升级

1. 从 [Releases](https://github.com/netsflex/VirtualMacOniPad/releases) 下载 `VirtualMac2_2.2.0beta2_1005_standalone.deb`，可用随附的 `SHA256SUMS` 校验。
2. 正常关闭所有虚拟机并退出任何版本的 Virtual Mac。已安装原版软件包 `com.mac.virtual` 时，需要先卸载原版。虚拟机文件存放于 `/var/mobile/Media/VirtualMac`，卸载原版会保留该目录中的虚拟机文件。
3. 在 iPad 上以 root 身份安装二进制文件，然后执行一次注销以加载键盘插件。

   ```bash
   dpkg -i VirtualMac2_2.2.0beta2_1005_standalone.deb
   ```

遇到问题时，请附上 iPad 型号、iPadOS 、 macOS 版本、复现步骤，以及导出的调试日志。日志收集开关可在应用设置中开启。 [报告问题](https://github.com/netsflex/VirtualMacOniPad/issues)

## 构建

在 Mac 上准备 Xcode（含对应版本的 macOS 和 iOS SDK）、Python 3 和 `ldid`。将下列两份上游包放在仓库根目录。它们是构建的输入文件，脚本会先核验 SHA-256，再提取所需组件并从本仓库编译安装二进制包。

| 上游包 | 用途 |
| --- | --- |
| [`VirtualMac_1.2.4.deb`](https://github.com/nfzerox/VirtualMacOniPad/releases/tag/1.2.4) | NAT 守护进程、依赖库和配置 | 
| [`VirtualMac_2.0.0beta1_1001.deb`](https://github.com/liukaixiang817/VirtualMacOniPad/releases/tag/2.0.0beta1) | Apple 框架、GPU 运行时、着色器缓存和应用资源 | 

在仓库根目录运行以下命令即可构建。 [构建脚本](VirtualMac/scripts/development/build-virtualmac2-release.sh)

```bash
bash VirtualMac/scripts/development/build-virtualmac2-release.sh \
  VirtualMac_1.2.4.deb VirtualMac_2.0.0beta1_1001.deb output \
  --release-version 2.2.0beta2 --build-number 1005
```

`output/` 中将生成 `VirtualMac2_2.2.0beta2_1005_standalone.deb`、`SHA256SUMS` 和 `release-manifest.json`。

## 上游与许可

本项目基于 [nfzerox/VirtualMacOniPad](https://github.com/nfzerox/VirtualMacOniPad) 和 [liukaixiang817/VirtualMacOniPad](https://github.com/liukaixiang817/VirtualMacOniPad)。感谢 [dsce](https://github.com/moraea/dsce)、[DyldExtractor](https://github.com/arandomdev/dyldextractor)、[MacWSBootingGuide](https://github.com/khanhduytran0/MacWSBootingGuide)、[HvDecompile](https://worthdoingbadly.com/hv/)、[UTM](https://github.com/utmapp/UTM)、[reims-vgpu](https://github.com/steelbrain/reims-vgpu)、[ipsw](https://github.com/blacktop/ipsw) 和 [VirtualBuddy](https://github.com/insidegui/VirtualBuddy) 等项目提供的研究和工具。

仓库源码采用 [MIT 许可](LICENSE)。从上游包提取的 Apple 框架不属于本仓库的 MIT 许可范围。
