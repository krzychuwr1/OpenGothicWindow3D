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

        private string adbPath;
        private string apkPath;
        private string questSerial;
        private string questModel;
        private bool   questUnauthorized;
        private bool   isInstalling;
        private bool   isPolling;

        public InstallerForm()
        {
            Text = "OpenGothic Window3D — Meta Quest Installer";
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
                Text = "Update app only (skip copying ~3.3 GB game files if already on Quest)",
                Location = new Point(14, 98),
                AutoSize = true,
                ForeColor = TextMuted
            };
            chkSkipFiles.CheckedChanged += (s, e) => UpdateInstallButton();
            cardGame.Controls.Add(chkSkipFiles);

            y += cardGame.Height + 14;

            // Card 2: Meta Quest connection
            var cardQuest = MakeCard(pad, y, contentW, 78);
            Controls.Add(cardQuest);

            cardQuest.Controls.Add(new Label
            {
                Text = "2. Meta Quest Headset (USB + Developer Mode)",
                Font = new Font("Segoe UI Semibold", 10.5f, FontStyle.Bold),
                ForeColor = TextPrimary,
                AutoSize = true,
                Location = new Point(14, 12)
            });

            lblQuestStatus = new Label
            {
                Text = "Checking for connected Meta Quest...",
                Location = new Point(14, 42),
                Size = new Size(contentW - 28, 24),
                ForeColor = TextMuted
            };
            cardQuest.Controls.Add(lblQuestStatus);

            y += cardQuest.Height + 14;

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
                string foundSerial = null;
                string foundModel = null;
                bool unauth = false;

                foreach (string raw in result.StdOut.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
                {
                    string line = raw.Trim();
                    if (line.StartsWith("List of devices")) continue;
                    var mAuth = Regex.Match(line, @"^(\S+)\s+device\s+.*\bmodel:((?:Meta_)?Quest(?:_\S+)?)(?:\s|$)");
                    if (mAuth.Success)
                    {
                        foundSerial = mAuth.Groups[1].Value;
                        foundModel = mAuth.Groups[2].Value.Replace('_', ' ');
                        break;
                    }
                    if (Regex.IsMatch(line, @"^\S+\s+unauthorized\b"))
                    {
                        unauth = true;
                    }
                }

                questSerial = foundSerial;
                questModel = foundModel;
                questUnauthorized = unauth;

                if (!string.IsNullOrEmpty(questSerial))
                {
                    lblQuestStatus.Text = string.Format("Connected: {0} ({1})", questModel, questSerial);
                    lblQuestStatus.ForeColor = OkColor;
                }
                else if (questUnauthorized)
                {
                    lblQuestStatus.Text = "Quest detected — put on your headset and click 'Allow USB debugging'.";
                    lblQuestStatus.ForeColor = WarnColor;
                }
                else
                {
                    lblQuestStatus.Text = "No Quest detected. Connect via USB cable with Developer Mode enabled.";
                    lblQuestStatus.ForeColor = TextMuted;
                }
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

        private void UpdateInstallButton()
        {
            bool hasQuest = !string.IsNullOrEmpty(questSerial);
            bool hasPrereqs = !string.IsNullOrEmpty(adbPath) && !string.IsNullOrEmpty(apkPath);
            bool hasGame = chkSkipFiles.Checked || IsValidGothicDir(txtGameDir.Text);

            btnInstall.Enabled = !isInstalling && hasQuest && hasPrereqs && hasGame;
            btnInstall.BackColor = btnInstall.Enabled ? AccentColor : Color.FromArgb(52, 57, 72);
            btnBrowse.Enabled = !isInstalling;
            chkSkipFiles.Enabled = !isInstalling;
            if (!isInstalling && hasQuest && btnLaunch.Tag != null)
                btnLaunch.Enabled = true;
        }

        private async void OnInstallClick(object sender, EventArgs e)
        {
            if (isInstalling) return;
            isInstalling = true;
            btnLaunch.Enabled = false;
            UpdateInstallButton();
            txtLog.Clear();
            progressBar.Value = 0;

            string serial = questSerial;
            string gameDir = txtGameDir.Text;
            bool skipFiles = chkSkipFiles.Checked;

            try
            {
                SetStep("Installing OpenGothicWindow3D.apk...", 5);
                AppendLog("Installing app onto " + questModel + "...");
                var rInstall = await RunProcessAsync(adbPath,
                    string.Format("-s {0} install -r \"{1}\"", serial, apkPath),
                    line => AppendLog(line));
                if (rInstall.ExitCode != 0)
                    throw new Exception("APK install failed:\n" + rInstall.StdErr);

                SetStep("Granting storage permission...", 12);
                await RunProcessAsync(adbPath,
                    string.Format("-s {0} shell appops set {1} MANAGE_EXTERNAL_STORAGE allow", serial, PackageName),
                    null);

                if (!skipFiles)
                {
                    await RunProcessAsync(adbPath,
                        string.Format("-s {0} shell mkdir -p {1}", serial, RemoteGameDir),
                        null);

                    var folders = new[]
                    {
                        new { Name = "Data",   Start = 15, End = 82 },
                        new { Name = "_work",  Start = 82, End = 95 },
                        new { Name = "System", Start = 95, End = 99 }
                    };

                    foreach (var f in folders)
                    {
                        string localSub = Path.Combine(gameDir, f.Name);
                        SetStep(string.Format("Copying {0} to Quest...", f.Name), f.Start);
                        AppendLog(string.Format("Copying {0} (this may take a couple of minutes)...", f.Name));

                        int startPct = f.Start;
                        int spanPct = f.End - f.Start;
                        var rPush = await RunProcessAsync(adbPath,
                            string.Format("-s {0} push --sync \"{1}\" \"{2}/\"", serial, localSub, RemoteGameDir),
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

                SetStep("Done! Open Library -> Unknown Sources -> OpenGothic Window3D on Quest.", 100);
                AppendLog("");
                AppendLog("Installation complete! You can now unplug your Quest or click 'Launch on Quest'.");
                btnLaunch.Tag = true;
                btnLaunch.Enabled = true;
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
            if (string.IsNullOrEmpty(questSerial) || string.IsNullOrEmpty(adbPath)) return;
            AppendLog("Launching OpenGothic Window3D on Quest...");
            await RunProcessAsync(adbPath,
                string.Format("-s {0} shell am start -n {1}/android.app.NativeActivity", questSerial, PackageName),
                line => AppendLog(line));
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
