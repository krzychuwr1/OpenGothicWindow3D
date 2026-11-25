using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace OpenGothicQuestInstaller
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new InstallerForm());
        }
    }

    sealed class InstallerForm : Form
    {
        private const string PackageName = "org.opengothic.window3d";
        private const string RemoteGameDir = "/sdcard/OpenGothic/Gothic2";
        // The app looks here first; needs no permission, but Meta Quest keeps adb-pushed files unreadable
        private const string AppGameDir    = "/sdcard/Android/data/" + PackageName + "/files/Gothic2";

        // Dark palette
        private static readonly Color BgColor      = Color.FromArgb(20, 22, 28);
        private static readonly Color CardColor    = Color.FromArgb(30, 33, 42);
        private static readonly Color InputBg      = Color.FromArgb(16, 18, 24);
        private static readonly Color BorderColor  = Color.FromArgb(52, 57, 72);
        private static readonly Color TextPrimary  = Color.FromArgb(240, 242, 248);
        private static readonly Color TextMuted    = Color.FromArgb(156, 164, 182);
        private static readonly Color AccentColor  = Color.FromArgb(72, 134, 245);
        private static readonly Color AccentHover  = Color.FromArgb(92, 150, 255);
        private static readonly Color OkColor      = Color.FromArgb(74, 210, 130);
        private static readonly Color WarnColor    = Color.FromArgb(242, 184, 75);
        private static readonly Color ErrColor     = Color.FromArgb(235, 92, 92);

        private readonly TextBox      txtGameDir;
        private readonly Button       btnBrowse;
        private readonly Label        lblGameStatus;
        private readonly CheckBox     chkSkipFiles;
        private readonly Label        lblQuestStatus;
        private readonly Label        lblStepStatus;
        private readonly ProgressBar  progressBar;
        private readonly TextBox      txtLog;
        private readonly Button       btnInstall;
        private readonly Button       btnLaunch;
        private readonly System.Windows.Forms.Timer pollTimer;

        // Advanced mode: any adb device, wireless adb, game file location, APK export
        private readonly CheckBox     chkAdvanced;
        private readonly Label        lblDeviceTitle;
        private readonly Panel        cardQuest;
        private readonly Panel        cardAdvanced;
        private readonly ComboBox     cmbDevice;
        private readonly TextBox      txtAddress;
        private readonly TextBox      txtPairCode;
        private readonly Button       btnConnect;
        private readonly RadioButton  rbShared;
        private readonly RadioButton  rbApp;
        private readonly CheckBox     chkInstallApp;
        private readonly Button       btnSaveApk;
        private readonly Button       btnSaveScript;
        private readonly List<Control> belowAdvanced = new List<Control>();

        private sealed class DeviceInfo
        {
            public string Serial;
            public string Model;
            public bool   IsQuest;
            public override string ToString()
            {
                string name = string.IsNullOrEmpty(Model) ? "Android device" : Model;
                return string.Format("{0} ({1}){2}", name, Serial, IsQuest ? "" : "  — not a Quest");
            }
        }

        private string adbPath;
        private string apkPath;
        private string lastExportDir;
        private List<DeviceInfo> devices = new List<DeviceInfo>();
        private bool   anyUnauthorized;
        private bool   isInstalling;
        private bool   isPolling;
        private bool   installedOnce;

        private bool Advanced { get { return chkAdvanced.Checked; } }

        public InstallerForm()
        {
            Text = "OpenGothic Window3D Installer";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            AutoScaleMode = AutoScaleMode.Dpi;
            BackColor = BgColor;
            ForeColor = TextPrimary;
            Font = new Font("Segoe UI", 9.5f, FontStyle.Regular);
            ClientSize = new Size(580, 560);

            try
            {
                Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            }
            catch { }

            int pad = 20;
            int contentW = ClientSize.Width - pad * 2;
            int y = 18;

            // Header
            var lblTitle = new Label
            {
                Text = "OpenGothic Window3D",
                Font = new Font("Segoe UI Semibold", 16f, FontStyle.Bold),
                ForeColor = TextPrimary,
                AutoSize = true,
                Location = new Point(pad, y)
            };
            Controls.Add(lblTitle);

            chkAdvanced = new CheckBox
            {
                Text = "Advanced (other headsets)",
                AutoSize = true,
                ForeColor = TextMuted,
                Location = new Point(pad + contentW - 182, y + 8)
            };
            Controls.Add(chkAdvanced);
            y += 32;

            var lblSub = new Label
            {
                Text = "Install the Quest app and copy your owned Gothic II: Night of the Raven files.",
                ForeColor = TextMuted,
                AutoSize = true,
                Location = new Point(pad, y)
            };
            Controls.Add(lblSub);
            y += 32;

            // Card 1: Gothic II folder
            var cardGame = MakeCard(pad, y, contentW, 134);
            Controls.Add(cardGame);

            cardGame.Controls.Add(new Label
            {
                Text = "1. Gothic II Installation",
                Font = new Font("Segoe UI Semibold", 10.5f, FontStyle.Bold),
                ForeColor = TextPrimary,
                AutoSize = true,
                Location = new Point(14, 12)
            });

            txtGameDir = new TextBox
            {
                Location = new Point(14, 40),
                Width = contentW - 118,
                BackColor = InputBg,
                ForeColor = TextPrimary,
                BorderStyle = BorderStyle.FixedSingle,
                ReadOnly = true
            };
            cardGame.Controls.Add(txtGameDir);

            btnBrowse = MakeSecondaryButton("Browse...", contentW - 96, 38, 82, 27);
            btnBrowse.Click += OnBrowseClick;
            cardGame.Controls.Add(btnBrowse);

            lblGameStatus = new Label
            {
                Location = new Point(14, 72),
                Size = new Size(contentW - 28, 22),
                ForeColor = TextMuted
            };
            cardGame.Controls.Add(lblGameStatus);

            chkSkipFiles = new CheckBox
            {
                Text = "Update app only (skip copying ~3.3 GB game files if already on the headset)",
                Location = new Point(14, 98),
                AutoSize = true,
                ForeColor = TextMuted
            };
            chkSkipFiles.CheckedChanged += (s, e) => UpdateInstallButton();
            cardGame.Controls.Add(chkSkipFiles);

            y += cardGame.Height + 14;

            // Card 2: Meta Quest connection
            cardQuest = MakeCard(pad, y, contentW, 78);
            Controls.Add(cardQuest);

            lblDeviceTitle = new Label
            {
                Text = "2. Meta Quest Headset (USB + Developer Mode)",
                Font = new Font("Segoe UI Semibold", 10.5f, FontStyle.Bold),
                ForeColor = TextPrimary,
                AutoSize = true,
                Location = new Point(14, 12)
            };
            cardQuest.Controls.Add(lblDeviceTitle);

            lblQuestStatus = new Label
            {
                Text = "Checking for connected Meta Quest...",
                Location = new Point(14, 42),
                Size = new Size(contentW - 28, 24),
                ForeColor = TextMuted
            };
            cardQuest.Controls.Add(lblQuestStatus);

            y += cardQuest.Height + 14;

            // Card 3 (advanced only): any adb device, wireless adb, file location, APK & script export
            cardAdvanced = MakeCard(pad, y, contentW, 228);
            cardAdvanced.Visible = false;
            Controls.Add(cardAdvanced);

            cardAdvanced.Controls.Add(new Label
            {
                Text = "3. Advanced",
                Font = new Font("Segoe UI Semibold", 10.5f, FontStyle.Bold),
                ForeColor = TextPrimary,
                AutoSize = true,
                Location = new Point(14, 12)
            });

            int labelW = 96;
            int fieldX = 14 + labelW;
            int fieldW = contentW - fieldX - 14;

            cardAdvanced.Controls.Add(MakeFieldLabel("Device:", 14, 43));
            cmbDevice = new ComboBox
            {
                Location = new Point(fieldX, 40),
                Width = fieldW,
                DropDownStyle = ComboBoxStyle.DropDownList,
                FlatStyle = FlatStyle.Flat,
                BackColor = InputBg,
                ForeColor = TextPrimary
            };
            cmbDevice.SelectedIndexChanged += (s, e) => { UpdateDeviceStatus(); UpdateInstallButton(); };
            cardAdvanced.Controls.Add(cmbDevice);

            cardAdvanced.Controls.Add(MakeFieldLabel("Wireless adb:", 14, 75));
            txtAddress = new TextBox
            {
                Location = new Point(fieldX, 72),
                Width = fieldW - 90 - 8 - 92 - 8,
                BackColor = InputBg,
                ForeColor = TextPrimary,
                BorderStyle = BorderStyle.FixedSingle
            };
            txtPairCode = new TextBox
            {
                Location = new Point(txtAddress.Right + 8, 72),
                Width = 92,
                BackColor = InputBg,
                ForeColor = TextPrimary,
                BorderStyle = BorderStyle.FixedSingle
            };
            btnConnect = MakeSecondaryButton("Connect", fieldX + fieldW - 90, 70, 90, 27);
            btnConnect.Click += OnConnectClick;
            txtPairCode.TextChanged += (s, e) =>
                btnConnect.Text = txtPairCode.Text.Trim().Length > 0 ? "Pair" : "Connect";
            cardAdvanced.Controls.Add(txtAddress);
            cardAdvanced.Controls.Add(txtPairCode);
            cardAdvanced.Controls.Add(btnConnect);
            var tips = new ToolTip();
            tips.SetToolTip(txtAddress, "IP address and port shown in the headset's wireless debugging settings, e.g. 192.168.1.20:37045");
            tips.SetToolTip(txtPairCode, "Pairing code: fill it in with the pairing address to pair first, leave empty to connect");
            cardAdvanced.Controls.Add(new Label
            {
                Text = "address:port            pairing code (optional)",
                Location = new Point(fieldX, 98),
                AutoSize = true,
                ForeColor = TextMuted,
                Font = new Font("Segoe UI", 8f)
            });

            cardAdvanced.Controls.Add(MakeFieldLabel("Game files to:", 14, 120));
            rbShared = new RadioButton
            {
                Text = "/sdcard/OpenGothic — shared folder, needs all-files access (Quest)",
                Location = new Point(fieldX, 118),
                AutoSize = true,
                ForeColor = TextMuted,
                Checked = true
            };
            rbApp = new RadioButton
            {
                Text = "The app's own folder — no permission, removed with the app",
                Location = new Point(fieldX, 140),
                AutoSize = true,
                ForeColor = TextMuted
            };
            cardAdvanced.Controls.Add(rbShared);
            cardAdvanced.Controls.Add(rbApp);

            chkInstallApp = new CheckBox
            {
                Text = "Install the app (untick to copy game files only)",
                Location = new Point(14, 164),
                AutoSize = true,
                ForeColor = TextMuted,
                Checked = true
            };
            chkInstallApp.CheckedChanged += (s, e) => UpdateInstallButton();
            cardAdvanced.Controls.Add(chkInstallApp);

            cardAdvanced.Controls.Add(MakeFieldLabel("Manual export:", 14, 195));
            btnSaveApk = MakeSecondaryButton("Save APK...", fieldX, 190, 110, 27);
            btnSaveApk.Click += OnSaveApkClick;
            tips.SetToolTip(btnSaveApk, "Save OpenGothicWindow3D.apk to install it some other way. No headset needed.");
            cardAdvanced.Controls.Add(btnSaveApk);

            btnSaveScript = MakeSecondaryButton("Save copy script...", fieldX + 118, 190, 146, 27);
            btnSaveScript.Click += OnSaveScriptClick;
            tips.SetToolTip(btnSaveScript, "Save copy-game-files.bat (Windows) and copy-game-files.sh (Linux/SteamOS) to push Data, _work and System over adb.");
            cardAdvanced.Controls.Add(btnSaveScript);

            chkAdvanced.CheckedChanged += (s, e) => OnAdvancedChanged();

            // Progress + Log
            lblStepStatus = new Label
            {
                Text = "Ready.",
                Location = new Point(pad, y),
                Size = new Size(contentW, 20),
                ForeColor = TextMuted
            };
            Controls.Add(lblStepStatus);
            y += 24;

            progressBar = new ProgressBar
            {
                Location = new Point(pad, y),
                Size = new Size(contentW, 14),
                Minimum = 0,
                Maximum = 100,
                Value = 0,
                Style = ProgressBarStyle.Continuous
            };
            Controls.Add(progressBar);
            y += 24;

            txtLog = new TextBox
            {
                Location = new Point(pad, y),
                Size = new Size(contentW, 128),
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                BackColor = InputBg,
                ForeColor = TextMuted,
                BorderStyle = BorderStyle.FixedSingle,
                Font = new Font("Consolas", 8.5f)
            };
            Controls.Add(txtLog);
            y += txtLog.Height + 14;

            // Bottom action buttons
            btnInstall = new Button
            {
                Text = "Install to Quest",
                Location = new Point(pad, y),
                Size = new Size(contentW - 150, 40),
                FlatStyle = FlatStyle.Flat,
                BackColor = AccentColor,
                ForeColor = Color.White,
                Font = new Font("Segoe UI Semibold", 10.5f, FontStyle.Bold),
                Cursor = Cursors.Hand,
                Enabled = false
            };
            btnInstall.FlatAppearance.BorderSize = 0;
            btnInstall.FlatAppearance.MouseOverBackColor = AccentHover;
            btnInstall.Click += OnInstallClick;
            Controls.Add(btnInstall);

            btnLaunch = MakeSecondaryButton("Launch on Quest", pad + contentW - 140, y, 140, 40);
            btnLaunch.Enabled = false;
            btnLaunch.Click += OnLaunchClick;
            Controls.Add(btnLaunch);

            belowAdvanced.AddRange(new Control[] { lblStepStatus, progressBar, txtLog, btnInstall, btnLaunch });

            // Locate prerequisites and auto-detect Gothic II
            LocatePrerequisites();
            string detected = FindGothic2();
            if (!string.IsNullOrEmpty(detected))
            {
                txtGameDir.Text = detected;
            }
            ValidateGameDir();

            pollTimer = new System.Windows.Forms.Timer { Interval = 2000 };
            pollTimer.Tick += async (s, e) => await PollQuestAsync();
            pollTimer.Start();

            Shown += async (s, e) => await PollQuestAsync();
        }

        private Panel MakeCard(int x, int y, int w, int h)
        {
            var p = new Panel
            {
                Location = new Point(x, y),
                Size = new Size(w, h),
                BackColor = CardColor
            };
            p.Paint += (s, e) =>
            {
                using (var pen = new Pen(BorderColor))
                    e.Graphics.DrawRectangle(pen, 0, 0, p.Width - 1, p.Height - 1);
            };
            return p;
        }

        private Label MakeFieldLabel(string text, int x, int y)
        {
            return new Label { Text = text, Location = new Point(x, y), AutoSize = true, ForeColor = TextMuted };
        }

        private void OnAdvancedChanged()
        {
            bool adv = Advanced;
            // Measured from the laid-out controls, so it stays right after DPI scaling
            int shift = cardAdvanced.Height + (cardAdvanced.Top - cardQuest.Bottom);
            SuspendLayout();
            cardAdvanced.Visible = adv;
            foreach (var c in belowAdvanced)
                c.Top += adv ? shift : -shift;
            ClientSize = new Size(ClientSize.Width, ClientSize.Height + (adv ? shift : -shift));
            ResumeLayout();

            lblDeviceTitle.Text = adv ? "2. Headset (any Android device over adb)" : "2. Meta Quest Headset (USB + Developer Mode)";
            btnInstall.Text = adv ? "Install to Headset" : "Install to Quest";
            btnLaunch.Text  = adv ? "Launch on Headset" : "Launch on Quest";
            if (!adv)
            {
                // The simple flow always installs the app and uses the shared folder
                chkInstallApp.Checked = true;
                rbShared.Checked = true;
            }
            UpdateDeviceStatus();
            UpdateInstallButton();
        }

        private Button MakeSecondaryButton(string text, int x, int y, int w, int h)
        {
            var b = new Button
            {
                Text = text,
                Location = new Point(x, y),
                Size = new Size(w, h),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(44, 49, 62),
                ForeColor = TextPrimary,
                Cursor = Cursors.Hand
            };
            b.FlatAppearance.BorderColor = BorderColor;
            b.FlatAppearance.BorderSize = 1;
            b.FlatAppearance.MouseOverBackColor = Color.FromArgb(58, 64, 82);
            return b;
        }

        private void LocatePrerequisites()
        {
            string exeDir = AppDomain.CurrentDomain.BaseDirectory;
            string extractedDir = ExtractEmbeddedPayload();

            // Find adb.exe: next to exe -> extracted embedded payload -> PATH
            string[] adbCandidates =
            {
                Path.Combine(exeDir, "platform-tools", "adb.exe"),
                extractedDir != null ? Path.Combine(extractedDir, "platform-tools", "adb.exe") : null
            };
            adbPath = adbCandidates.FirstOrDefault(p => !string.IsNullOrEmpty(p) && File.Exists(p))
                      ?? FindInPath("adb.exe");

            // Find APK: next to exe -> extracted embedded payload -> repo dev build fallback
            string[] apkCandidates =
            {
                Path.Combine(exeDir, "OpenGothicWindow3D.apk"),
                extractedDir != null ? Path.Combine(extractedDir, "OpenGothicWindow3D.apk") : null,
                Path.GetFullPath(Path.Combine(exeDir, "..", "..", "build", "quest", "OpenGothicWindow3D.apk"))
            };
            apkPath = apkCandidates.FirstOrDefault(p => !string.IsNullOrEmpty(p) && File.Exists(p));

            if (string.IsNullOrEmpty(adbPath))
                AppendLog("ERROR: adb.exe not found.");
            if (string.IsNullOrEmpty(apkPath))
                AppendLog("ERROR: OpenGothicWindow3D.apk not found.");
        }

        private string ExtractEmbeddedPayload()
        {
            try
            {
                var asm = Assembly.GetExecutingAssembly();
                using (var stream = asm.GetManifestResourceStream("payload.zip"))
                {
                    if (stream == null) return null;

                    var fi = new FileInfo(Application.ExecutablePath);
                    string stamp = fi.Length.ToString() + "_" + fi.LastWriteTimeUtc.Ticks.ToString();
                    string dir = Path.Combine(Path.GetTempPath(), "OpenGothicWindow3D-Installer", stamp);
                    string marker = Path.Combine(dir, ".ready");

                    if (!File.Exists(marker))
                    {
                        if (Directory.Exists(dir))
                            Directory.Delete(dir, true);
                        Directory.CreateDirectory(dir);
                        using (var archive = new ZipArchive(stream, ZipArchiveMode.Read))
                        {
                            archive.ExtractToDirectory(dir);
                        }
                        File.WriteAllText(marker, "ok");
                    }
                    return dir;
                }
            }
            catch (Exception ex)
            {
                AppendLog("ERROR: Failed to extract bundled files: " + ex.Message);
                return null;
            }
        }

        private static string FindInPath(string exe)
        {
            string pathEnv = Environment.GetEnvironmentVariable("PATH") ?? "";
            foreach (string dir in pathEnv.Split(';'))
            {
                if (string.IsNullOrWhiteSpace(dir)) continue;
                try
                {
                    string full = Path.Combine(dir.Trim(), exe);
                    if (File.Exists(full)) return full;
                }
                catch { }
            }
            return null;
        }

        private static bool IsValidGothicDir(string dir)
        {
            if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir)) return false;
            return Directory.Exists(Path.Combine(dir, "Data")) &&
                   Directory.Exists(Path.Combine(dir, "_work")) &&
                   Directory.Exists(Path.Combine(dir, "System"));
        }

        private static string FindGothic2()
        {
            var candidates = new List<string>();

            // Steam library folders
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                {
                    string steam = key != null ? key.GetValue("SteamPath") as string : null;
                    if (!string.IsNullOrEmpty(steam))
                    {
                        steam = steam.Replace('/', '\\');
                        var libs = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { steam };
                        string vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
                        if (File.Exists(vdf))
                        {
                            string text = File.ReadAllText(vdf);
                            foreach (Match m in Regex.Matches(text, "\"path\"\\s+\"([^\"]+)\""))
                            {
                                libs.Add(m.Groups[1].Value.Replace("\\\\", "\\"));
                            }
                        }
                        foreach (string lib in libs)
                            candidates.Add(Path.Combine(lib, "steamapps", "common", "Gothic II"));
                    }
                }
            }
            catch { }

            // GOG registry entries
            string[] gogRoots =
            {
                @"SOFTWARE\WOW6432Node\GOG.com\Games",
                @"SOFTWARE\GOG.com\Games"
            };
            foreach (string root in gogRoots)
            {
                try
                {
                    using (var rk = Registry.LocalMachine.OpenSubKey(root))
                    {
                        if (rk == null) continue;
                        foreach (string sub in rk.GetSubKeyNames())
                        {
                            using (var gk = rk.OpenSubKey(sub))
                            {
                                if (gk == null) continue;
                                string name = gk.GetValue("gameName") as string ?? "";
                                string path = gk.GetValue("path") as string ?? "";
                                if ((name.StartsWith("Gothic II", StringComparison.OrdinalIgnoreCase) ||
                                     name.StartsWith("Gothic 2", StringComparison.OrdinalIgnoreCase)) &&
                                    !string.IsNullOrEmpty(path))
                                {
                                    candidates.Add(path);
                                }
                            }
                        }
                    }
                }
                catch { }
            }

            candidates.Add(@"C:\Program Files (x86)\Steam\steamapps\common\Gothic II");
            candidates.Add(@"C:\GOG Games\Gothic 2 Gold");
            candidates.Add(@"D:\GOG Games\Gothic 2 Gold");

            foreach (string c in candidates)
            {
                if (IsValidGothicDir(c)) return c;
            }
            return null;
        }

        private void ValidateGameDir()
        {
            string dir = txtGameDir.Text;
            if (!IsValidGothicDir(dir))
            {
                lblGameStatus.Text = "Not found — click Browse and select your Gothic II folder (containing Data, _work, System).";
                lblGameStatus.ForeColor = ErrColor;
            }
            else if (!File.Exists(Path.Combine(dir, "Data", "Worlds_Addon.vdf")))
            {
                lblGameStatus.Text = "Warning: Worlds_Addon.vdf missing — Gothic II Gold / Night of the Raven is required.";
                lblGameStatus.ForeColor = WarnColor;
            }
            else
            {
                lblGameStatus.Text = "Gothic II: Night of the Raven detected.";
                lblGameStatus.ForeColor = OkColor;
            }
            UpdateInstallButton();
        }

        private void OnBrowseClick(object sender, EventArgs e)
        {
            using (var dlg = new FolderBrowserDialog())
            {
                dlg.Description = "Select your Gothic II installation folder (containing Data, _work, and System)";
                dlg.ShowNewFolderButton = false;
                if (Directory.Exists(txtGameDir.Text))
                    dlg.SelectedPath = txtGameDir.Text;
                if (dlg.ShowDialog(this) == DialogResult.OK)
                {
                    txtGameDir.Text = dlg.SelectedPath;
                    ValidateGameDir();
                }
            }
        }

        private async Task PollQuestAsync()
        {
            if (isInstalling || isPolling || string.IsNullOrEmpty(adbPath))
                return;
            isPolling = true;
            try
            {
                var result = await RunProcessAsync(adbPath, "devices -l", null);
                var found = new List<DeviceInfo>();
                bool unauth = false;

                foreach (string raw in result.StdOut.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
                {
                    string line = raw.Trim();
                    if (line.StartsWith("List of devices")) continue;
                    var m = Regex.Match(line, @"^(\S+)\s+device(?:\s|$)");
                    if (m.Success)
                    {
                        var mm = Regex.Match(line, @"\bmodel:(\S+)");
                        string model = mm.Success ? mm.Groups[1].Value : "";
                        found.Add(new DeviceInfo
                        {
                            Serial  = m.Groups[1].Value,
                            Model   = model.Replace('_', ' '),
                            IsQuest = Regex.IsMatch(model, @"^(?:Meta_)?Quest(?:_\S+)?$")
                        });
                    }
                    else if (Regex.IsMatch(line, @"^\S+\s+unauthorized\b"))
                    {
                        unauth = true;
                    }
                }

                devices = found;
                anyUnauthorized = unauth;
                RefreshDeviceList();
                UpdateDeviceStatus();
                UpdateInstallButton();
            }
            catch
            {
                lblQuestStatus.Text = "Could not query adb.";
                lblQuestStatus.ForeColor = ErrColor;
            }
            finally
            {
                isPolling = false;
            }
        }

        // Keeps the advanced device list in step with adb without losing the user's choice
        private void RefreshDeviceList()
        {
            var current = cmbDevice.SelectedItem as DeviceInfo;
            var shown = cmbDevice.Items.Cast<DeviceInfo>().Select(d => d.Serial + "|" + d.Model);
            var now = devices.Select(d => d.Serial + "|" + d.Model);
            if (shown.SequenceEqual(now)) return;

            cmbDevice.BeginUpdate();
            cmbDevice.Items.Clear();
            foreach (var d in devices) cmbDevice.Items.Add(d);
            int keep = current == null ? -1 : devices.FindIndex(d => d.Serial == current.Serial);
            if (keep < 0) keep = devices.FindIndex(d => d.IsQuest);
            if (keep < 0 && devices.Count > 0) keep = 0;
            cmbDevice.SelectedIndex = keep;
            cmbDevice.EndUpdate();
        }

        // Simple mode: the first connected Quest. Advanced mode: whatever is picked in the list.
        private DeviceInfo SelectedDevice()
        {
            if (Advanced)
                return cmbDevice.SelectedItem as DeviceInfo;
            return devices.FirstOrDefault(d => d.IsQuest);
        }

        private void UpdateDeviceStatus()
        {
            var dev = SelectedDevice();
            if (dev != null)
            {
                string name = string.IsNullOrEmpty(dev.Model) ? "Android device" : dev.Model;
                lblQuestStatus.Text = string.Format("Connected: {0} ({1})", name, dev.Serial);
                lblQuestStatus.ForeColor = OkColor;
                if (Advanced && !dev.IsQuest)
                    lblQuestStatus.Text += " — not a Quest; compatibility is untested";
            }
            else if (anyUnauthorized)
            {
                lblQuestStatus.Text = "Headset detected — put it on and allow USB debugging.";
                lblQuestStatus.ForeColor = WarnColor;
            }
            else if (!Advanced && devices.Count > 0)
            {
                lblQuestStatus.Text = "An Android device is connected, but no Quest. For other headsets tick Advanced.";
                lblQuestStatus.ForeColor = WarnColor;
            }
            else
            {
                lblQuestStatus.Text = Advanced
                    ? "No device. Connect over USB with USB debugging allowed, or use wireless adb below."
                    : "No Quest detected. Connect via USB cable with Developer Mode enabled.";
                lblQuestStatus.ForeColor = TextMuted;
            }
        }

        private void UpdateInstallButton()
        {
            bool hasDevice = SelectedDevice() != null;
            bool installApp = chkInstallApp.Checked;
            bool hasPrereqs = !string.IsNullOrEmpty(adbPath) && (!installApp || !string.IsNullOrEmpty(apkPath));
            bool hasGame = chkSkipFiles.Checked || IsValidGothicDir(txtGameDir.Text);
            bool hasWork = installApp || !chkSkipFiles.Checked;

            btnInstall.Enabled = !isInstalling && hasDevice && hasPrereqs && hasGame && hasWork;
            btnInstall.BackColor = btnInstall.Enabled ? AccentColor : Color.FromArgb(52, 57, 72);
            btnBrowse.Enabled = !isInstalling;
            chkSkipFiles.Enabled = !isInstalling;
            chkAdvanced.Enabled = !isInstalling;
            cardAdvanced.Enabled = !isInstalling;
            btnSaveApk.Enabled = !string.IsNullOrEmpty(apkPath);
            btnLaunch.Enabled = !isInstalling && hasDevice && (installedOnce || Advanced);
        }

        private async void OnInstallClick(object sender, EventArgs e)
        {
            var dev = SelectedDevice();
            if (isInstalling || dev == null) return;
            isInstalling = true;
            UpdateInstallButton();
            txtLog.Clear();
            progressBar.Value = 0;

            string serial = dev.Serial;
            string deviceName = string.IsNullOrEmpty(dev.Model) ? serial : dev.Model;
            string gameDir = txtGameDir.Text;
            bool skipFiles = chkSkipFiles.Checked;
            bool installApp = chkInstallApp.Checked;
            bool sharedTarget = rbShared.Checked;
            string remoteDir = sharedTarget ? RemoteGameDir : AppGameDir;

            try
            {
                if (installApp)
                {
                    SetStep("Installing OpenGothicWindow3D.apk...", 5);
                    AppendLog("Installing app onto " + deviceName + "...");
                    var rInstall = await RunProcessAsync(adbPath,
                        string.Format("-s {0} install -r \"{1}\"", serial, apkPath),
                        line => AppendLog(line));
                    if (rInstall.ExitCode != 0)
                    {
                        string detail = string.IsNullOrWhiteSpace(rInstall.StdErr) ? rInstall.StdOut.Trim() : rInstall.StdErr.Trim();
                        throw new Exception("APK install failed:\n" + detail);
                    }
                }

                if (sharedTarget)
                {
                    SetStep("Granting storage permission...", 12);
                    var rPerm = await RunProcessAsync(adbPath,
                        string.Format("-s {0} shell appops set {1} MANAGE_EXTERNAL_STORAGE allow", serial, PackageName),
                        null);
                    if (rPerm.ExitCode != 0)
                    {
                        string detail = string.IsNullOrWhiteSpace(rPerm.StdErr) ? rPerm.StdOut.Trim() : rPerm.StdErr.Trim();
                        AppendLog("WARNING: could not grant all-files access" +
                                  (detail.Length > 0 ? " (" + detail + ")" : "") +
                                  ". Allow it for the app in the headset's settings, or copy to the app's own folder (Advanced).");
                    }
                }
                else if (dev.IsQuest)
                {
                    AppendLog("WARNING: on Meta Quest the app cannot read files copied into its own folder; use the shared folder.");
                }

                if (!skipFiles)
                {
                    if (!installApp && !sharedTarget)
                        AppendLog("Note: the app must already be installed, so that its folder belongs to it.");
                    var rMkdir = await RunProcessAsync(adbPath,
                        string.Format("-s {0} shell mkdir -p {1}", serial, remoteDir),
                        null);
                    if (rMkdir.ExitCode != 0)
                    {
                        string detail = string.IsNullOrWhiteSpace(rMkdir.StdErr) ? rMkdir.StdOut.Trim() : rMkdir.StdErr.Trim();
                        throw new Exception("Could not create " + remoteDir + " on the device:\n" + detail);
                    }

                    var folders = new[]
                    {
                        new { Name = "Data",   Start = 15, End = 82 },
                        new { Name = "_work",  Start = 82, End = 95 },
                        new { Name = "System", Start = 95, End = 99 }
                    };

                    foreach (var f in folders)
                    {
                        string localSub = Path.Combine(gameDir, f.Name);
                        SetStep(string.Format("Copying {0} to {1}...", f.Name, deviceName), f.Start);
                        AppendLog(string.Format("Copying {0} to {1} (this may take a couple of minutes)...", f.Name, remoteDir));

                        int startPct = f.Start;
                        int spanPct = f.End - f.Start;
                        var rPush = await RunProcessAsync(adbPath,
                            string.Format("-s {0} push --sync \"{1}\" \"{2}/\"", serial, localSub, remoteDir),
                            line =>
                            {
                                var m = Regex.Match(line, @"\[\s*(\d+)%\]");
                                if (m.Success)
                                {
                                    int p;
                                    if (int.TryParse(m.Groups[1].Value, out p))
                                    {
                                        int mapped = startPct + (p * spanPct) / 100;
                                        SetStep(string.Format("Copying {0}... {1}%", f.Name, p), mapped);
                                    }
                                }
                                else if (line.Contains("pushed"))
                                {
                                    AppendLog(line);
                                }
                            });
                        if (rPush.ExitCode != 0)
                            throw new Exception("Copying " + f.Name + " failed:\n" + rPush.StdErr);
                    }
                }

                if (dev.IsQuest)
                    SetStep("Done! Open Library -> Unknown Sources -> OpenGothic Window3D on Quest.", 100);
                else
                    SetStep("Done! Start OpenGothic Window3D from the headset's app library.", 100);
                AppendLog("");
                AppendLog("Installation complete! You can now unplug the headset or click '" + btnLaunch.Text + "'.");
                installedOnce = true;
            }
            catch (Exception ex)
            {
                SetStep("Installation failed.", progressBar.Value);
                AppendLog("ERROR: " + ex.Message);
                MessageBox.Show(this, ex.Message, "Installation Failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                isInstalling = false;
                UpdateInstallButton();
            }
        }

        private async void OnLaunchClick(object sender, EventArgs e)
        {
            var dev = SelectedDevice();
            if (dev == null || string.IsNullOrEmpty(adbPath)) return;
            AppendLog("Launching OpenGothic Window3D...");
            await RunProcessAsync(adbPath,
                string.Format("-s {0} shell am start -n {1}/android.app.NativeActivity", dev.Serial, PackageName),
                line => AppendLog(line));
        }

        private async void OnConnectClick(object sender, EventArgs e)
        {
            if (string.IsNullOrEmpty(adbPath)) return;
            string address = txtAddress.Text.Trim();
            string code = txtPairCode.Text.Trim();
            // Both go on the adb command line: allow only host:port and a numeric code
            if (!Regex.IsMatch(address, @"^(?:[A-Za-z0-9.\-]+|\[[0-9A-Fa-f:]+\]):\d{1,5}$"))
            {
                AppendLog("Enter the address as shown on the headset, e.g. 192.168.1.20:37045");
                return;
            }
            if (code.Length > 0 && !Regex.IsMatch(code, @"^\d{4,10}$"))
            {
                AppendLog("The pairing code is the number shown on the headset.");
                return;
            }
            btnConnect.Enabled = false;
            try
            {
                bool pairing = code.Length > 0;
                string args = pairing ? string.Format("pair {0} {1}", address, code)
                                      : string.Format("connect {0}", address);
                AppendLog("adb " + (pairing ? "pair " : "connect ") + address + " ...");
                var r = await RunProcessAsync(adbPath, args, line => AppendLog(line));
                if (pairing && r.ExitCode == 0)
                {
                    txtPairCode.Clear();
                    AppendLog("Now enter the address from the wireless debugging screen itself and click Connect.");
                }
            }
            finally
            {
                btnConnect.Enabled = true;
            }
            await PollQuestAsync();
        }

        private void OnSaveApkClick(object sender, EventArgs e)
        {
            if (string.IsNullOrEmpty(apkPath)) return;
            using (var dlg = new SaveFileDialog())
            {
                dlg.FileName = "OpenGothicWindow3D.apk";
                dlg.Filter = "Android package (*.apk)|*.apk";
                dlg.OverwritePrompt = true;
                if (!string.IsNullOrEmpty(lastExportDir) && Directory.Exists(lastExportDir))
                    dlg.InitialDirectory = lastExportDir;
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    File.Copy(apkPath, dlg.FileName, true);
                    lastExportDir = Path.GetDirectoryName(dlg.FileName);
                    AppendLog("Saved " + dlg.FileName);
                    AppendLog("After installing it another way, copy Data, _work and System from your Gothic II folder to");
                    AppendLog("  " + RemoteGameDir + " (then allow the app all-files access), or to");
                    AppendLog("  " + AppGameDir + " (no permission needed).");
                    AppendLog("Or click 'Save copy script...' to generate ready-to-run .bat and .sh scripts.");
                }
                catch (Exception ex)
                {
                    AppendLog("ERROR: could not save the APK: " + ex.Message);
                }
            }
        }

        private void OnSaveScriptClick(object sender, EventArgs e)
        {
            using (var dlg = new SaveFileDialog())
            {
                dlg.FileName = "copy-game-files.bat";
                dlg.Filter = "Windows batch script (*.bat)|*.bat";
                dlg.OverwritePrompt = true;
                if (!string.IsNullOrEmpty(lastExportDir) && Directory.Exists(lastExportDir))
                    dlg.InitialDirectory = lastExportDir;
                if (dlg.ShowDialog(this) != DialogResult.OK) return;

                string dir = Path.GetDirectoryName(dlg.FileName);
                string batPath = dlg.FileName;
                string shPath = Path.Combine(dir, Path.GetFileNameWithoutExtension(dlg.FileName) + ".sh");
                bool sharedTarget = rbShared.Checked;
                string remoteDir = sharedTarget ? RemoteGameDir : AppGameDir;
                string gameDir = IsValidGothicDir(txtGameDir.Text) ? txtGameDir.Text.Trim().TrimEnd('\\', '/') : "";
                string batGameDir = gameDir.Replace("%", "%%");

                var bat = new StringBuilder();
                bat.AppendLine("@echo off");
                bat.AppendLine("setlocal");
                bat.AppendLine("rem Copies Gothic II game files (Data, _work, System) to an Android OpenXR headset over adb.");
                bat.AppendLine("rem Usage: copy-game-files.bat [\"C:\\Path\\To\\Gothic II\"]   (set ANDROID_SERIAL to pick a device)");
                bat.AppendLine("");
                bat.AppendLine("set \"GAME_DIR=%~1\"");
                if (batGameDir.Length > 0)
                    bat.AppendLine("if \"%GAME_DIR%\"==\"\" set \"GAME_DIR=" + batGameDir + "\"");
                bat.AppendLine("if \"%GAME_DIR%\"==\"\" (");
                bat.AppendLine("  echo Usage: %~nx0 \"C:\\Path\\To\\Gothic II\"");
                bat.AppendLine("  pause");
                bat.AppendLine("  exit /b 1");
                bat.AppendLine(")");
                bat.AppendLine("");
                bat.AppendLine("set \"ADB=adb\"");
                bat.AppendLine("if exist \"%~dp0platform-tools\\adb.exe\" set \"ADB=%~dp0platform-tools\\adb.exe\"");
                bat.AppendLine("where \"%ADB%\" >nul 2>nul || if not exist \"%ADB%\" (");
                bat.AppendLine("  echo ERROR: adb not found on PATH or in platform-tools next to this script.");
                bat.AppendLine("  pause");
                bat.AppendLine("  exit /b 1");
                bat.AppendLine(")");
                bat.AppendLine("");
                bat.AppendLine("for %%D in (Data _work System) do (");
                bat.AppendLine("  if not exist \"%GAME_DIR%\\%%D\" (");
                bat.AppendLine("    echo ERROR: Missing \"%GAME_DIR%\\%%D\" - select your Gothic II Gold / Night of the Raven folder.");
                bat.AppendLine("    pause");
                bat.AppendLine("    exit /b 1");
                bat.AppendLine("  )");
                bat.AppendLine(")");
                bat.AppendLine("");
                bat.AppendLine("set \"TARGET=" + remoteDir + "\"");
                bat.AppendLine("echo Creating %TARGET% on headset...");
                bat.AppendLine("\"%ADB%\" shell mkdir -p \"%TARGET%\" || goto :fail");
                bat.AppendLine("for %%D in (Data _work System) do (");
                bat.AppendLine("  echo Copying %%D to %TARGET%...");
                bat.AppendLine("  \"%ADB%\" push --sync \"%GAME_DIR%\\%%D\" \"%TARGET%/\" || goto :fail");
                bat.AppendLine(")");
                if (sharedTarget)
                {
                    bat.AppendLine("echo Granting all-files access...");
                    bat.AppendLine("\"%ADB%\" shell appops set " + PackageName + " MANAGE_EXTERNAL_STORAGE allow || echo WARNING: could not grant all-files access; allow it in the headset's app settings.");
                }
                bat.AppendLine("echo.");
                bat.AppendLine("echo Done! Game files copied to %TARGET%.");
                bat.AppendLine("pause");
                bat.AppendLine("exit /b 0");
                bat.AppendLine("");
                bat.AppendLine(":fail");
                bat.AppendLine("echo.");
                bat.AppendLine("echo ERROR: Copy failed. Check that the headset is connected and USB debugging is allowed.");
                bat.AppendLine("pause");
                bat.AppendLine("exit /b 1");

                var sh = new StringBuilder();
                sh.Append("#!/usr/bin/env bash\n");
                sh.Append("# Copies Gothic II game files (Data, _work, System) to an Android OpenXR headset over adb.\n");
                sh.Append("# Usage: ./copy-game-files.sh /path/to/Gothic2   (set ANDROID_SERIAL to pick a device)\n");
                sh.Append("set -euo pipefail\n\n");
                sh.Append("GAME_DIR=\"${1:-}\"\n");
                sh.Append("if [ -z \"$GAME_DIR\" ]; then\n");
                sh.Append("  echo \"Usage: $0 /path/to/Gothic2\" >&2\n");
                sh.Append("  exit 1\n");
                sh.Append("fi\n\n");
                sh.Append("if ! command -v adb >/dev/null 2>&1; then\n");
                sh.Append("  echo \"ERROR: adb not found on PATH.\" >&2\n");
                sh.Append("  exit 1\n");
                sh.Append("fi\n\n");
                sh.Append("for d in Data _work System; do\n");
                sh.Append("  if [ ! -d \"$GAME_DIR/$d\" ]; then\n");
                sh.Append("    echo \"ERROR: Missing $GAME_DIR/$d - point to your Gothic II Gold / Night of the Raven folder.\" >&2\n");
                sh.Append("    exit 1\n");
                sh.Append("  fi\n");
                sh.Append("done\n\n");
                sh.Append("TARGET=\"" + remoteDir + "\"\n");
                sh.Append("echo \"Creating $TARGET on headset...\"\n");
                sh.Append("adb shell mkdir -p \"$TARGET\"\n");
                sh.Append("for d in Data _work System; do\n");
                sh.Append("  echo \"Copying $d to $TARGET...\"\n");
                sh.Append("  adb push --sync \"$GAME_DIR/$d\" \"$TARGET/\"\n");
                sh.Append("done\n");
                if (sharedTarget)
                {
                    sh.Append("echo \"Granting all-files access...\"\n");
                    sh.Append("adb shell appops set " + PackageName + " MANAGE_EXTERNAL_STORAGE allow || echo \"WARNING: could not grant all-files access; allow it in the headset's app settings.\"\n");
                }
                sh.Append("echo \"Done! Game files copied to $TARGET.\"\n");

                try
                {
                    var utf8NoBom = new UTF8Encoding(false);
                    File.WriteAllText(batPath, bat.ToString(), utf8NoBom);
                    File.WriteAllText(shPath, sh.ToString(), utf8NoBom);
                    lastExportDir = dir;
                    AppendLog("Saved " + batPath);
                    AppendLog("Saved " + shPath);
                    AppendLog("Target baked in: " + remoteDir + (sharedTarget ? " (with all-files access grant)" : " (app's own folder)"));
                }
                catch (Exception ex)
                {
                    AppendLog("ERROR: could not save the copy scripts: " + ex.Message);
                }
            }
        }

        private void SetStep(string text, int pct)
        {
            if (InvokeRequired)
            {
                BeginInvoke((Action)(() => SetStep(text, pct)));
                return;
            }
            lblStepStatus.Text = text;
            lblStepStatus.ForeColor = pct >= 100 ? OkColor : TextPrimary;
            progressBar.Value = Math.Max(progressBar.Minimum, Math.Min(progressBar.Maximum, pct));
        }

        private void AppendLog(string line)
        {
            if (string.IsNullOrWhiteSpace(line)) return;
            if (InvokeRequired)
            {
                BeginInvoke((Action)(() => AppendLog(line)));
                return;
            }
            txtLog.AppendText(line.TrimEnd() + Environment.NewLine);
        }

        private sealed class ProcessResult
        {
            public int ExitCode;
            public string StdOut;
            public string StdErr;
        }

        private static Task<ProcessResult> RunProcessAsync(string fileName, string args, Action<string> onLine)
        {
            return Task.Run(() =>
            {
                var stdout = new StringBuilder();
                var stderr = new StringBuilder();

                var psi = new ProcessStartInfo
                {
                    FileName = fileName,
                    Arguments = args,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };

                using (var proc = new Process { StartInfo = psi })
                {
                    proc.OutputDataReceived += (s, e) =>
                    {
                        if (e.Data == null) return;
                        stdout.AppendLine(e.Data);
                        if (onLine != null) onLine(e.Data);
                    };
                    proc.ErrorDataReceived += (s, e) =>
                    {
                        if (e.Data == null) return;
                        stderr.AppendLine(e.Data);
                        if (onLine != null) onLine(e.Data);
                    };

                    proc.Start();
                    proc.BeginOutputReadLine();
                    proc.BeginErrorReadLine();
                    proc.WaitForExit();

                    return new ProcessResult
                    {
                        ExitCode = proc.ExitCode,
                        StdOut = stdout.ToString(),
                        StdErr = stderr.ToString()
                    };
                }
            });
        }
    }
}
