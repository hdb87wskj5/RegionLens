# RegionLens 1.3.0 · 区域镜

[简体中文](#简体中文) | [English](#english)

## 简体中文

区域镜可以把 Windows 屏幕上的选定区域显示在可移动、可缩放或全屏的实时窗口中。支持多个区域、截图、画质设置和受安全恢复机制保护的鼠标映射。

设置窗口是普通非置顶窗口，可正常截图和框选；打开期间仍可新建区域、使用鼠标映射和全局快捷键。仅录入快捷键或提交配置时进行必要的短时保护。

### 下载与安装

目前**没有公开安装包**。现有 1.3.0 安装包使用仅面向本地和熟人测试的自签名证书；其安装过程会请求管理员权限，并把证书加入 Windows 的受信任根证书颁发机构和受信任发布者存储，因此暂不面向公众分发。

完整鼠标映射需要签名的程序从受保护的安装目录启动，以满足 Windows UIAccess 要求。本仓库只跟踪源码，不把 EXE、私钥、证书或运行日志提交到 Git 历史中。

### 从源码构建

需要 Visual Studio 2026（C++ 桌面工具、v145）、Windows SDK 10.0.26100.0 和 Windows PowerShell。在仓库根目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Unsigned.ps1 -Configuration Debug
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Unsigned.ps1 -Configuration Release
```

脚本编译正式版应用、微信输入法兼容 DLL，并使用假输入后端运行核心测试。产物位于 `out/`；脚本不会创建或安装证书，也不会向真实桌面注入测试鼠标操作。**无签名构建仅适合开发与审查，不具备完整的 UIAccess 鼠标映射部署能力。**自行打包时请使用自己控制的签名证书，并审查安装器的证书信任行为。

### 源码与许可

- `src/Core`：捕获、渲染、窗口、输入映射与安全恢复。
- `src/App`：正式版入口和资源。
- `src/WeTypeProbe`：经过身份核验的微信输入法候选框兼容组件；不支持的版本会静默停用。
- `src/Setup`：安装与卸载源码。`tests`：核心自动测试，不包含鼠标测试窗口或运行时诊断记录器。

源码对应的内部版本见 [`SOURCE_PROVENANCE.md`](SOURCE_PROVENANCE.md)。项目原创代码及袋鼠图标采用 [MIT 许可证](LICENSE)；AMD CAS 改编着色器保留其 MIT 声明，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

## English

RegionLens mirrors a selected part of the Windows desktop into a movable, resizable, or full-screen live window. It supports multiple regions, screenshots, image-quality controls, and guarded mouse mapping.

Settings is an ordinary non-topmost, capturable window. Region creation, mouse mapping, and global shortcuts remain available while it is open; brief protection applies only when recording shortcuts or committing settings.

### Download and install

There is **no public installer yet**. The existing 1.3.0 installer uses a self-signed certificate intended for local and small-group testing. Installation requests administrator approval and adds that certificate to the Windows Trusted Root Certification Authorities and Trusted Publishers stores, so it is not being distributed publicly.

Full mouse mapping requires a signed executable launched from a secure installation location under Windows UIAccess rules. This Git repository tracks source only; executables, private keys, certificates, and runtime logs are not committed to Git history.

### Build from source

You need Visual Studio 2026 (Desktop development with C++, v145), Windows SDK 10.0.26100.0, and Windows PowerShell. From the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Unsigned.ps1 -Configuration Debug
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Unsigned.ps1 -Configuration Release
```

The script builds the Stable app and WeType compatibility DLL, then runs core tests with fake input backends. Output is under `out/`. It does not create or trust certificates, install the app, or inject test mouse input into the real desktop. **An unsigned build is for development and review, not a full UIAccess mouse-mapping deployment.** Package your own build only with a certificate you control and after reviewing the installer's trust behavior.

### Source and license

- `src/Core`: capture, rendering, windows, input mapping, and safe recovery.
- `src/App`: Stable entry point and resources.
- `src/WeTypeProbe`: identity-checked WeType candidate-window compatibility component; unsupported versions remain inactive.
- `src/Setup`: installer and uninstaller source. `tests`: core tests, with no mouse test receiver or runtime diagnostic writer.

See [`SOURCE_PROVENANCE.md`](SOURCE_PROVENANCE.md) for the maintained source commit. Original project code and the supplied kangaroo icon are [MIT licensed](LICENSE). The adapted AMD CAS shader retains its MIT notice; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
