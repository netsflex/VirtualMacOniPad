# Task 编译输出的可选持久缓存

缓存代码位于 `vz/host/modern_pvg_shader_persistent.inc`。只有真实原生
library 创建成功、NSError 为 nil 后才发布；损坏、权限或身份不匹配、
可选预算耗尽时继续使用原转换流程。它不证明 GPU 像素或 UI 已正常。

只构建 Task 单库，使用已核验的两项输入：

```sh
bash scripts/development/build-modern-gputask-compat.sh \
  /absolute/path/ModernRuntimeCompat.dylib \
  /absolute/path/MetalCompat.dylib /absolute/path/fresh-output
```

不要为此调用 `build-virtualmac2.sh` 重建 CPU/VMM 或其他框架。
保存源码、两项依赖、SDK/clang/参数、实际平台与最低系统版本、产物 SHA256、
CDHash、签名/dyld 验证及导出列表。新产物应单独审核，不能直接当成已测试版本。

缓存按编译器集合划分，不再绑定单台设备。profile 目录名是源码内七项 compiler
文件名、SHA256、CDHash、大小及 profile 哈希的 SHA256；更换 compiler 版本后表
变化，旧条目不会被查到。启用前只做廉价检查：`compiler27` 为 root:wheel 0755，
七项文件为 root:wheel 0755、单链接且大小与表一致。不再记录 inode、时间戳或
st_dev，因此重装、重启和重新越狱后缓存继续有效，也不需要收据或逐设备重建 Task。
这项检查不重新计算 124 MB compiler 的内容哈希；内容由安装脚本的 SHA256 核对
负责，拥有 root 权限并换入等大小文件的情形不在防护范围内。

缓存位于 `/var/root/VirtualMac2/shader-cache/converter-v2/<compiler 集合哈希>`，
由 mobile 的 GPU Task 首次使用时创建，权限 0700，无需安装脚本或手工部署。
上限为 8192 项、1 GiB、单库 2 MiB；写入前的配额扫描与条目数成正比，发布预算
为 1 秒并在 utility 队列执行，不占用 worker.lock。回滚只需恢复原 Task。

原生 confstr 缓存有独立的 `native_pvg_cache_identity.h` scope。普通 Task
重启、endpoint 或这次缓存代码调整不应生成新 scope；实际 native ABI 输入变化
才另行生成 scope，并保留旧目录内容。每次真实验证必须分别记录缓存 hit、原生
library 结果、GPU completion/像素和 UI 响应，不能相互替代。

完整着色器输入采集默认关闭。需要复现编译问题时，仅在诊断 helper 的环境中
设置 `VZ_PVG_SHADER_INPUT_AUDIT=1`；其他值不会安装采集包装。静态转换缓存、
持久缓存和原生编译回退独立保留。该开关避免每次 library 创建后同步复制、
哈希及写入诊断输入，不代表图标生成、App 首帧或 GPU 性能已验证。
