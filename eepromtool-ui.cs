// SPDX-License-Identifier: GPL-3.0-only
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;

internal sealed class EepromWindow : Form
{
    private const int MaxImageBytes = 128 * 1024;
    private const string BinaryName = "eepromtool.exe";

    private static readonly Regex AdapterLine = new Regex(
        @"^\s*-\s+(\\Device\\NPF_\{[0-9a-fA-F-]+\})\s+\((.*)\)\s*$",
        RegexOptions.Multiline | RegexOptions.IgnoreCase);

    private readonly string toolPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, BinaryName);
    private readonly ComboBox adapter = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly NumericUpDown slave = new NumericUpDown { Minimum = 1, Maximum = 65535, Value = 1, Width = 86 };
    private readonly TextBox imagePath = new TextBox { Dock = DockStyle.Fill, ReadOnly = true };
    private readonly TextBox log = new TextBox
    {
        Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, WordWrap = true,
        ScrollBars = ScrollBars.Vertical, BackColor = SystemColors.Window
    };
    private readonly Label status = new Label { Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
    private readonly Button refresh = MakeButton("刷新网卡");
    private readonly Button browse = MakeButton("选择镜像…");
    private readonly Button read = MakeButton("读取信息");
    private readonly Button backup = MakeButton("备份 EEPROM…");
    private readonly Button write = MakeButton("写入并校验…");
    private bool busy;

    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new EepromWindow());
    }

    private EepromWindow()
    {
        Text = "EtherCAT EEPROM 工具";
        Font = new Font("Microsoft YaHei UI", 10);
        AutoScaleMode = AutoScaleMode.Font;
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(660, 455);
        ClientSize = new Size(800, 480);

        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(16), ColumnCount = 3, RowCount = 6 };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        for (int row = 0; row < 5; row++)
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = false };
        actions.Controls.AddRange(new Control[] { read, backup, write });
        AddRow(layout, 0, "有线网卡", adapter, refresh);
        AddRow(layout, 1, "从站序号", slave, null);
        AddRow(layout, 2, "SII 镜像", imagePath, browse);
        AddFullRow(layout, actions, 3);
        AddFullRow(layout, status, 4);
        AddFullRow(layout, log, 5);
        status.AutoSize = true;
        status.Padding = new Padding(0, 6, 0, 6);
        Controls.Add(layout);

        string projectImage = Path.GetFullPath(Path.Combine(
            AppDomain.CurrentDomain.BaseDirectory, @"..\..\Firmware-Rust\lib\soes-esi\eeprom.bin"));
        if (File.Exists(projectImage))
            imagePath.Text = projectImage;

        adapter.SelectedIndexChanged += delegate { UpdateButtons(); };
        refresh.Click += async delegate { await RefreshAdaptersAsync(); };
        browse.Click += delegate { SelectImage(); };
        read.Click += async delegate { await ReadInfoAsync(); };
        backup.Click += async delegate { await BackupAsync(); };
        write.Click += async delegate { await WriteAsync(); };
        Shown += async delegate { await RefreshAdaptersAsync(); };
        FormClosing += delegate(object sender, FormClosingEventArgs args)
        {
            if (busy)
            {
                args.Cancel = true;
                MessageBox.Show(this, "操作正在运行，请等它完成。", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
        };
        UpdateButtons();
    }

    private static Button MakeButton(string caption)
    {
        return new Button { Text = caption, AutoSize = true, Padding = new Padding(9, 3, 9, 3) };
    }

    private static void AddRow(TableLayoutPanel layout, int row, string title, Control field, Control button)
    {
        var label = new Label { Text = title, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 0, 10, 0) };
        layout.Controls.Add(label, 0, row);
        field.Anchor = AnchorStyles.Left | (field.Dock == DockStyle.Fill ? AnchorStyles.Right : AnchorStyles.None);
        layout.Controls.Add(field, 1, row);
        if (button != null)
            layout.Controls.Add(button, 2, row);
    }

    private static void AddFullRow(TableLayoutPanel layout, Control control, int row)
    {
        layout.Controls.Add(control, 0, row);
        layout.SetColumnSpan(control, 3);
    }

    private void UpdateButtons()
    {
        bool selected = !busy && adapter.SelectedItem is NetworkAdapter;
        adapter.Enabled = refresh.Enabled = browse.Enabled = slave.Enabled = !busy;
        read.Enabled = backup.Enabled = selected;
        write.Enabled = selected && imagePath.Text.Length != 0;
    }

    private async Task DuringOperation(string initialStatus, Func<Task> operation)
    {
        busy = true;
        UpdateButtons();
        status.Text = initialStatus;
        log.Clear();
        try
        {
            await operation();
        }
        catch (Exception ex)
        {
            status.Text = "操作失败";
            AppendLog(ex.Message);
        }
        finally
        {
            busy = false;
            UpdateButtons();
        }
    }

    private void AppendLog(string line)
    {
        if (!string.IsNullOrWhiteSpace(line))
            log.AppendText(line.TrimEnd() + Environment.NewLine);
    }

    private Task RefreshAdaptersAsync()
    {
        var previous = adapter.SelectedItem as NetworkAdapter;
        return DuringOperation("正在刷新网卡…", async delegate
        {
            string output = await RunToolAsync("");
            var adapters = new List<NetworkAdapter>();
            foreach (Match match in AdapterLine.Matches(output))
                adapters.Add(new NetworkAdapter(match.Groups[1].Value, match.Groups[2].Value));

            adapter.Items.Clear();
            adapter.Items.Add("请选择连接板卡的有线网卡");
            adapter.Items.AddRange(adapters.OrderBy(item => item.Description).ToArray());
            adapter.SelectedIndex = 0;
            if (previous != null)
            {
                NetworkAdapter current = adapters.FirstOrDefault(item => item.Id == previous.Id);
                if (current != null)
                    adapter.SelectedItem = current;
            }
            status.Text = adapters.Count > 0
                ? "请选择接到 EtherCAT IN 的网卡。"
                : "未找到可用网卡。请检查 Npcap 和网卡状态。";
        });
    }

    private void SelectImage()
    {
        using (var dialog = new OpenFileDialog
        {
            Title = "选择 EtherCAT SII 镜像",
            Filter = "BIN 镜像 (*.bin)|*.bin",
            FileName = imagePath.Text,
            CheckFileExists = true,
            RestoreDirectory = true
        })
        {
            if (dialog.ShowDialog(this) == DialogResult.OK)
                imagePath.Text = dialog.FileName;
        }
        UpdateButtons();
    }

    private string TargetArguments()
    {
        var selected = (NetworkAdapter)adapter.SelectedItem;
        return selected.Id + " " + slave.Value.ToString(CultureInfo.InvariantCulture);
    }

    private Task ReadInfoAsync()
    {
        string target = TargetArguments();
        return DuringOperation("正在读取从站信息…", async delegate
        {
            await RunToolAsync(target + " -i");
            status.Text = "EEPROM 信息读取完成。";
        });
    }

    private Task BackupAsync()
    {
        using (var dialog = new SaveFileDialog
        {
            Title = "保存 EEPROM 备份",
            Filter = "BIN 镜像 (*.bin)|*.bin",
            FileName = "eeprom-backup.bin",
            DefaultExt = "bin",
            AddExtension = true,
            OverwritePrompt = true,
            RestoreDirectory = true
        })
        {
            if (dialog.ShowDialog(this) != DialogResult.OK)
                return Task.FromResult(0);

            string destination = dialog.FileName;
            string target = TargetArguments();
            return DuringOperation("正在备份 EEPROM…", async delegate
            {
                using (var work = new ScratchDirectory())
                {
                    await RunToolAsync(target + " -r readback.bin", work.Location);
                    byte[] bytes = File.ReadAllBytes(Path.Combine(work.Location, "readback.bin"));
                    if (DeclaredImageLength(bytes) != bytes.Length)
                        throw new IOException("读回文件长度与 EEPROM 声明的容量不一致。");
                    await Task.Run(() => SaveBackup(destination, bytes));
                }
                status.Text = "备份已保存。";
                AppendLog(destination);
            });
        }
    }

    private Task WriteAsync()
    {
        var selected = (NetworkAdapter)adapter.SelectedItem;
        string target = TargetArguments();
        string path = imagePath.Text;
        decimal slaveNumber = slave.Value;

        return DuringOperation("正在检查镜像和目标从站…", async delegate
        {
            byte[] expected = await Task.Run(() => LoadImage(path));
            string info = await RunToolAsync(target + " -i");
            var fields = new Regex(@"^\s*(?:找到 \d+ 个 EtherCAT 从站。|(?:厂商 ID|产品码|修订号|序列号)\s+[0-9A-F]+)\s*$", RegexOptions.Multiline);
            string identity = string.Join(Environment.NewLine, fields.Matches(info).Cast<Match>().Select(match => match.Value.Trim()));
            string message = string.Format(
                "网卡  {0}\n从站序号  {1}\n镜像  {2}\n镜像大小  {3:N0} 字节\n\n当前从站\n{4}\n\n此操作将覆盖目标从站 EEPROM。确认写入？",
                selected.Description, slaveNumber, path, expected.Length, identity);
            var choice = MessageBox.Show(this, message, "确认写入 EEPROM", MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
            if (choice != DialogResult.Yes)
            {
                status.Text = "已取消写入。";
                return;
            }

            using (var work = new ScratchDirectory())
            {
                File.WriteAllBytes(Path.Combine(work.Location, "image.bin"), expected);
                try
                {
                    status.Text = "正在写入 EEPROM，请保持板卡供电…";
                    await RunToolAsync(target + " -w image.bin", work.Location);
                    status.Text = "正在读回并逐字节校验…";
                    await RunToolAsync(target + " -r readback.bin", work.Location);
                    byte[] actual = File.ReadAllBytes(Path.Combine(work.Location, "readback.bin"));
                    if (!actual.SequenceEqual(expected))
                        throw new IOException("读回内容与所选镜像不一致。");
                    status.Text = "校验通过。";
                    AppendLog("读回内容与镜像逐字节一致。现在可以给板卡断电重启。");
                }
                catch (Exception ex)
                {
                    status.Text = "写入或校验失败";
                    AppendLog(ex.Message);
                    AppendLog("请保持板卡供电，检查日志和连接。EEPROM 可能已部分写入。");
                }
            }
        });
    }

    private static int DeclaredImageLength(byte[] bytes)
    {
        if (bytes.Length < 128)
            throw new IOException("文件不足 128 字节，缺少完整的 SII 头。");
        return ((bytes[124] | (bytes[125] << 8)) + 1) * 128;
    }

    private static byte[] LoadImage(string path)
    {
        using (var stream = File.OpenRead(path))
        using (var reader = new BinaryReader(stream))
        {
            if (stream.Length < 128 || stream.Length > MaxImageBytes)
                throw new IOException("镜像长度超出 SII EEPROM 支持的范围。");
            byte[] bytes = reader.ReadBytes((int)stream.Length);
            if (DeclaredImageLength(bytes) != bytes.Length)
                throw new IOException("镜像长度和 SII 容量字段不一致。");

            int crc = 0xff;
            for (int offset = 0; offset < 14; offset++)
            {
                crc ^= bytes[offset];
                for (int bit = 0; bit < 8; bit++)
                    crc = ((crc << 1) ^ ((crc & 0x80) != 0 ? 0x07 : 0)) & 0xff;
            }
            if (crc != bytes[14])
                throw new IOException("SII 头校验和不正确，请选择有效的 EEPROM BIN 镜像。");
            return bytes;
        }
    }

    private static void SaveBackup(string destination, byte[] bytes)
    {
        string staging = Path.Combine(Path.GetDirectoryName(destination), Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            File.WriteAllBytes(staging, bytes);
            if (File.Exists(destination))
                File.Replace(staging, destination, null);
            else
                File.Move(staging, destination);
        }
        finally
        {
            File.Delete(staging);
        }
    }

    private async Task<string> RunToolAsync(string arguments, string workingDirectory = null)
    {
        if (!File.Exists(toolPath))
            throw new IOException("找不到 eepromtool.exe。请把命令行程序和图形界面放在同一目录。");

        var start = new ProcessStartInfo(toolPath, arguments)
        {
            WorkingDirectory = workingDirectory ?? Path.GetDirectoryName(toolPath),
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        using (var process = Process.Start(start))
        {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> errors = process.StandardError.ReadToEndAsync();
            await Task.Run(() => process.WaitForExit());
            string text = await output;
            if (arguments.Length != 0 || process.ExitCode != 0)
                AppendLog(text);
            AppendLog(await errors);
            if (unchecked((uint)process.ExitCode) == 0xc0000135)
                throw new IOException("缺少 Npcap 运行库。请使用 WinPcap API 兼容模式安装 Npcap。");
            if (process.ExitCode != 0)
                throw new IOException("eepromtool 执行失败。请查看上方日志。");
            return text;
        }
    }

    private sealed class NetworkAdapter
    {
        public readonly string Id;
        public readonly string Description;

        public NetworkAdapter(string id, string description)
        {
            Id = id;
            Description = description;
        }

        public override string ToString()
        {
            return Description + "   " + Id;
        }
    }

    private sealed class ScratchDirectory : IDisposable
    {
        public readonly string Location = Path.Combine(Path.GetTempPath(), "ecio-eeprom-" + Guid.NewGuid().ToString("N"));

        public ScratchDirectory()
        {
            Directory.CreateDirectory(Location);
        }

        public void Dispose()
        {
            try { Directory.Delete(Location, true); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}
