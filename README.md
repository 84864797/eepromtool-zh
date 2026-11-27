# eepromtool 中文版

基于 [SOEM v2.0.1](https://github.com/OpenEtherCATsociety/SOEM/releases/tag/v2.0.1) 的 EtherCAT SII EEPROM 读写工具。Windows 中文界面支持读取信息、备份 BIN、写入前确认，以及写入后自动读回并逐字节校验。命令行版保留原有参数。

## Windows 使用

1. 从 [Releases](https://github.com/84864797/eepromtool-zh/releases) 下载 Windows x64 压缩包并解压。将两个 EXE 放在同一目录。
2. 安装 [Npcap](https://npcap.com/#download)，勾选 **Install Npcap in WinPcap API-compatible Mode**。如果限制管理员使用 Npcap，请以管理员身份运行工具。
3. 用电脑有线网口直连板卡的 EtherCAT IN 口并给板卡上电。如果 TwinCAT 等主站使用同一网口，先停止该主站。
4. 双击 `eepromtool-ui.exe`，选择实际接线的网卡。直连一块板卡时，从站序号填 `1`。点击「读取信息」确认目标。已有配置的设备可以用「备份 EEPROM」保存一份 BIN。
5. 点击「选择镜像」打开板卡对应的 SII `eeprom.bin`，再点击「写入并校验」。确认框会显示网卡、从站和镜像。看到「校验通过」后，可以给设备断电重启。

新设备的 EEPROM 若没有有效的容量字段，工具会在备份时显示无法确定长度。读取信息仍可用于确认从站是否可访问。

### PowerShell 命令行

运行 `./eepromtool.exe` 可列出网卡及全部选项。把示例中的网卡名称替换为实际列表中的有线网卡。

```powershell
$nic = '\Device\NPF_{网卡GUID}'
./eepromtool.exe $nic 1 -i
./eepromtool.exe $nic 1 -r ./eeprom-backup.bin
```

下列命令会将已选定的 SII 镜像写入目标从站，并读回对比 SHA-256。请先核对镜像及从站序号。

```powershell
$image = (Resolve-Path -LiteralPath './eeprom.bin').ProviderPath
./eepromtool.exe $nic 1 -w $image
if ($LASTEXITCODE -ne 0) { throw 'EEPROM 写入失败' }
./eepromtool.exe $nic 1 -r ./readback.bin
if ($LASTEXITCODE -ne 0) { throw 'EEPROM 读回失败' }
Get-FileHash $image, ./readback.bin
```

## 构建

在 Windows x64 PowerShell 中运行：

```powershell
./build.ps1 -Zig 'C:/path/to/zig.exe'
./build-gui.ps1
```

命令行版使用 Zig 0.15.2，脚本会校验并解压随仓库提供的 SOEM 源码包。图形界面使用 Windows .NET Framework 的 C# 编译器。Npcap 是运行时依赖，单独从官网下载。

发行包对应的源码见 [GitHub 源码仓库](https://github.com/84864797/eepromtool-zh)。本项目按 [GPL-3.0](LICENSE) 发布，保留 [SOEM 上游许可声明](SOEM-LICENSE.md)。
