// Beyond Good & Evil Enhanced Launcher
// Builds with the .NET Framework csc that ships with Windows (C# 5 syntax only).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

namespace BGELauncher
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }

    // ---------------------------------------------------------------- simple ini
    class Ini
    {
        public readonly string Path;
        readonly Dictionary<string, string> d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public Ini(string path)
        {
            Path = path;
            if (File.Exists(path))
                foreach (var raw in File.ReadAllLines(path))
                {
                    var l = raw.Trim();
                    if (l.Length == 0 || l.StartsWith(";") || l.StartsWith("[")) continue;
                    int i = l.IndexOf('=');
                    if (i > 0) d[l.Substring(0, i).Trim()] = l.Substring(i + 1).Trim();
                }
        }
        public string S(string k, string def) { string v; return d.TryGetValue(k, out v) ? v : def; }
        public int I(string k, int def) { int v; return int.TryParse(S(k, ""), out v) ? v : def; }
        public bool B(string k, bool def) { return I(k, def ? 1 : 0) != 0; }
        public void Set(string k, object v) { d[k] = v is bool ? (((bool)v) ? "1" : "0") : Convert.ToString(v, System.Globalization.CultureInfo.InvariantCulture); }
        public void Save(string section)
        {
            var sb = new StringBuilder();
            sb.AppendLine("[" + section + "]");
            foreach (var kv in d) sb.AppendLine(kv.Key + "=" + kv.Value);
            File.WriteAllText(Path, sb.ToString());
        }
    }

    // ---------------------------------------------------------------- main window
    class MainForm : Form
    {
        static readonly string Root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        static string Payload { get { return System.IO.Path.Combine(Root, "payload"); } }
        const string RegGame = @"Software\Ubisoft\Beyond Good & Evil";
        const string RegVideo = @"Software\Ubisoft\Beyond Good & Evil\SettingsApplication.INI\Basic video";
        string GameDir;

        Ini cfg;

        // Display
        ComboBox cbRes, cbWindow, cbVsync, cbFps, cbHud, cbToggleKey;
        CheckBox chkWide, chkFlipPadX, chkFlipPadY, chkFlipMouseX, chkFlipMouseY;
        TrackBar tbFov; Label lblFov;
        // Game graphics
        ComboBox cbWater, cbBuffers, cbAniso, cbMsaa;
        TrackBar tbShadow; Label lblShadow;
        CheckBox chkFlare, chkAfterFx, chkBlur, chkMip, chkHwVp;
        // Enhancements
        RadioButton rbNative, rbDxvk;
        CheckBox chkRcas, chkSmaa, chkMxao, chkGi, chkDeband, chkDsoal;
        ComboBox cbSpeakers;
        TrackBar tbSharp; Label lblSharp;
        GroupBox gbReshade;
        Label lblImmerse;
        Label lblStatus;

        public MainForm()
        {
            Text = "Beyond Good & Evil - Enhanced Launcher";
            ClientSize = new Size(640, 560);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 9f);
            BackColor = Color.FromArgb(24, 28, 26);
            ForeColor = Color.WhiteSmoke;
            try { Icon = Icon.ExtractAssociatedIcon(FindGame() + "\\BGE.exe"); } catch { }

            GameDir = FindGame();
            cfg = new Ini(System.IO.Path.Combine(Root, "launcher.ini"));

            var header = new Label
            {
                Text = "BEYOND GOOD && EVIL", Font = new Font("Segoe UI Semibold", 18f),
                ForeColor = Color.FromArgb(120, 200, 160), AutoSize = true, Location = new Point(16, 10)
            };
            var sub = new Label
            {
                Text = "Enhanced launcher  ·  60 fps lock · widescreen · FSR sharpening · SMAA · ambient occlusion · GI",
                AutoSize = true, Location = new Point(19, 46), ForeColor = Color.FromArgb(185, 196, 190)
            };
            Controls.Add(header); Controls.Add(sub);

            var tabs = new TabControl { Location = new Point(12, 72), Size = new Size(616, 410) };
            tabs.TabPages.Add(BuildDisplayTab());
            tabs.TabPages.Add(BuildGraphicsTab());
            tabs.TabPages.Add(BuildEnhanceTab());
            tabs.TabPages.Add(BuildDarkroomTab());
            tabs.TabPages.Add(BuildToolsTab());
            Controls.Add(tabs);

            lblStatus = new Label { Location = new Point(16, 494), Size = new Size(400, 54), ForeColor = Color.FromArgb(185, 196, 190) };
            Controls.Add(lblStatus);

            var btnSave = MkButton("Save", new Point(420, 500), 90);
            btnSave.Click += delegate { if (ApplyAll()) SetStatus("Settings saved."); };
            var btnPlay = MkButton("PLAY", new Point(518, 492), 110);
            btnPlay.Height = 44; btnPlay.Font = new Font("Segoe UI Semibold", 12f);
            btnPlay.BackColor = Color.FromArgb(60, 140, 100);
            btnPlay.Click += delegate { Play(); };
            Controls.Add(btnSave); Controls.Add(btnPlay);

            LoadValues();
            UpdateEnables();
            if (GameDir == null) SetStatus("Game folder not found - set it on the Tools tab.");
            else SetStatus("Game: " + GameDir);
        }

        // ------------------------------------------------------------ UI helpers
        Button MkButton(string t, Point p, int w)
        {
            return new Button { Text = t, Location = p, Width = w, Height = 30, FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(50, 58, 54), ForeColor = Color.White };
        }
        TabPage Page(string t) { return new TabPage(t) { BackColor = Color.FromArgb(36, 42, 39), ForeColor = Color.WhiteSmoke }; }
        Label L(Control parent, string text, int x, int y, int w = 0)
        {
            var l = new Label { Text = text, Location = new Point(x, y + 3), AutoSize = w == 0 };
            if (w > 0) l.Size = new Size(w, 18);
            parent.Controls.Add(l); return l;
        }
        ComboBox Combo(Control parent, int x, int y, int w, params string[] items)
        {
            var c = new ComboBox { Location = new Point(x, y), Width = w, DropDownStyle = ComboBoxStyle.DropDownList };
            c.Items.AddRange(items); parent.Controls.Add(c); return c;
        }
        CheckBox Check(Control parent, string t, int x, int y, string tip = null)
        {
            var c = new CheckBox { Text = t, Location = new Point(x, y), AutoSize = true };
            parent.Controls.Add(c);
            if (tip != null) new ToolTip().SetToolTip(c, tip);
            return c;
        }
        Label Note(Control parent, string t, int x, int y, int w, int h)
        {
            var l = new Label { Text = t, Location = new Point(x, y), Size = new Size(w, h), ForeColor = Color.FromArgb(185, 196, 190) };
            parent.Controls.Add(l); return l;
        }
        void SetStatus(string s) { lblStatus.Text = s; }

        // ------------------------------------------------------------ tabs
        TabPage BuildDisplayTab()
        {
            var p = Page("Display");
            int y = 16;
            L(p, "Resolution", 16, y); cbRes = Combo(p, 170, y, 200);
            foreach (var r in ScreenModes()) cbRes.Items.Add(r);
            y += 34;
            L(p, "Window mode", 16, y); cbWindow = Combo(p, 170, y, 200, "Fullscreen", "Windowed", "Borderless fullscreen");
            L(p, "Switch key", 380, y); cbToggleKey = Combo(p, 450, y, 140, "Alt+Enter", "F11", "Ctrl+Enter", "Alt+F", "None");
            new ToolTip().SetToolTip(cbToggleKey, "In game, switches between borderless fullscreen and a window. Works when Window mode is Windowed or Borderless.");
            y += 34;
            L(p, "Frame rate cap", 16, y); cbFps = Combo(p, 170, y, 200, "60 fps (recommended)", "30 fps", "120 fps (may break physics)", "144 fps (may break physics)", "Unlimited (breaks physics)");
            Note(p, "The Jade engine's physics and animation are tuned for 60 fps. Your 144 Hz display would otherwise run it too fast.", 380, y - 4, 220, 60);
            y += 34;
            L(p, "V-Sync", 16, y); cbVsync = Combo(p, 170, y, 200, "Off (cap handles pacing)", "On", "Game default");
            y += 48;
            chkWide = Check(p, "Widescreen fix (Hor+, removes 4:3 stretch)", 16, y);
            y += 30;
            L(p, "Field of view", 16, y); tbFov = new TrackBar { Location = new Point(170, y - 4), Width = 200, Minimum = 80, Maximum = 130, TickFrequency = 10, SmallChange = 1, LargeChange = 5 };
            p.Controls.Add(tbFov); lblFov = L(p, "100%", 380, y); tbFov.ValueChanged += delegate { lblFov.Text = tbFov.Value + "%"; };
            y += 44;
            L(p, "HUD / menus", 16, y); cbHud = Combo(p, 170, y, 200, "Stretched to screen", "4:3 centred (correct shape)");
            y += 40;
            L(p, "Controller camera", 16, y);
            chkFlipPadX = Check(p, "Flip left/right", 170, y, "Reverses the right stick's horizontal camera axis only.");
            chkFlipPadY = Check(p, "Flip up/down", 300, y, "Reverses the right stick's vertical camera axis only.");
            y += 28;
            L(p, "Mouse camera", 16, y);
            chkFlipMouseX = Check(p, "Flip left/right", 170, y);
            chkFlipMouseY = Check(p, "Flip up/down", 300, y);
            Note(p, "Applied on top of the game's own Invert option, which flips both axes together. Leave the in-game option off and pick the axis here.", 16, y + 28, 580, 32);
            Note(p, "Fixes are applied live by bgehook (d3d9.dll) - the game files are not modified.", 16, y + 62, 560, 20);
            return p;
        }

        TabPage BuildGraphicsTab()
        {
            var p = Page("Graphics");
            int y = 16;
            L(p, "Texture filtering", 16, y); cbAniso = Combo(p, 170, y, 200, "Game default (trilinear)", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x");
            y += 34;
            L(p, "Anti-aliasing (MSAA)", 16, y); cbMsaa = Combo(p, 170, y, 200, "Off", "2x MSAA", "4x MSAA", "8x MSAA");
            var msaaWarn = Note(p, "Not recommended: forced MSAA can cause black screens or broken graphics later in the game. Use SMAA (Vulkan) instead.", 380, y - 6, 220, 46);
            msaaWarn.ForeColor = Color.FromArgb(240, 190, 110);
            cbMsaa.SelectedIndexChanged += delegate
            {
                if (cbMsaa.Focused && cbMsaa.SelectedIndex > 0)
                    SetStatus("Warning: forced MSAA is known to cause black screens or corrupted graphics at some points in BG&&E. If that happens, set it back to Off.");
            };
            y += 44;
            L(p, "Water quality", 16, y); cbWater = Combo(p, 170, y, 200, "Low", "Medium", "High");
            y += 34;
            L(p, "Shadow quality", 16, y); tbShadow = new TrackBar { Location = new Point(170, y - 4), Width = 200, Minimum = 0, Maximum = 255, TickFrequency = 32, LargeChange = 32 };
            p.Controls.Add(tbShadow); lblShadow = L(p, "128", 380, y); tbShadow.ValueChanged += delegate { lblShadow.Text = tbShadow.Value.ToString(); };
            y += 44;
            L(p, "Frame buffers", 16, y); cbBuffers = Combo(p, 170, y, 200, "Double buffering", "Triple buffering");
            y += 40;
            chkFlare = Check(p, "Lens flare", 16, y);
            chkAfterFx = Check(p, "After effects (glow, colour FX)", 200, y);
            y += 28;
            chkBlur = Check(p, "Blur / motion blur", 16, y);
            chkMip = Check(p, "Mipmapping", 200, y);
            y += 28;
            chkHwVp = Check(p, "Hardware vertex processing", 16, y, "Faster on modern GPUs.");
            Note(p, "These are the game's own settings (same as SettingsApplication.exe), stored in the registry.", 16, y + 40, 560, 20);
            return p;
        }

        TabPage BuildEnhanceTab()
        {
            var p = Page("Enhancements");
            var gbR = new GroupBox { Text = "Renderer", Location = new Point(10, 8), Size = new Size(588, 56), ForeColor = Color.WhiteSmoke };
            rbNative = new RadioButton { Text = "Native Direct3D 9", Location = new Point(12, 22), AutoSize = true };
            rbDxvk = new RadioButton { Text = "Vulkan (DXVK) - needed for SMAA / MXAO / GI", Location = new Point(160, 22), AutoSize = true };
            foreach (var rb in new[] { rbNative, rbDxvk }) { gbR.Controls.Add(rb); rb.CheckedChanged += delegate { UpdateEnables(); }; }
            p.Controls.Add(gbR);

            gbReshade = new GroupBox { Text = "ReShade post-processing  (Home key = in-game menu)", Location = new Point(10, 72), Size = new Size(588, 130), ForeColor = Color.WhiteSmoke };
            chkRcas = Check(gbReshade, "AMD FSR 1 RCAS sharpening", 12, 22);
            L(gbReshade, "Strength", 230, 22);
            tbSharp = new TrackBar { Location = new Point(290, 16), Width = 180, Height = 26, AutoSize = false, Minimum = 0, Maximum = 20, TickFrequency = 2, TickStyle = TickStyle.None };
            gbReshade.Controls.Add(tbSharp); lblSharp = L(gbReshade, "", 476, 22);
            tbSharp.ValueChanged += delegate { lblSharp.Text = (tbSharp.Value * 5) + "%"; };
            chkSmaa = Check(gbReshade, "SMAA anti-aliasing", 12, 50);
            chkDeband = Check(gbReshade, "Deband (smooth sky gradients)", 300, 50);
            chkMxao = Check(gbReshade, "MXAO ambient occlusion", 12, 76);
            chkGi = Check(gbReshade, "Global illumination - NiceGuy SSGI", 300, 76);
            Note(gbReshade, "SMAA, MXAO and GI need the Vulkan renderer. GI is screen-space (ray marched in the depth buffer).", 12, 100, 560, 20);
            p.Controls.Add(gbReshade);

            var gbI = new GroupBox { Text = "iMMERSE shaders (SMAA, MXAO, and motion vectors for GI)", Location = new Point(10, 210), Size = new Size(588, 96), ForeColor = Color.WhiteSmoke };
            Note(gbI, "These are Pascal \"Marty McFly\" Gilcher's free iMMERSE shaders. Their licence doesn't allow bundling, so the launcher downloads them from the official GitHub page the first time you use them.", 12, 20, 560, 34);
            lblImmerse = L(gbI, "", 12, 60, 380);
            var dl = MkButton("Download / update", new Point(420, 56), 150); dl.Height = 28;
            dl.Click += delegate { if (GameDir != null) { SetStatus(DownloadImmerse() ? "iMMERSE shaders installed." : "iMMERSE download failed."); UpdateImmerseLabel(); } };
            gbI.Controls.Add(dl);
            p.Controls.Add(gbI);

            var gbA = new GroupBox { Text = "3D sound and EAX reverb (DSOAL)", Location = new Point(10, 312), Size = new Size(588, 62), ForeColor = Color.WhiteSmoke };
            chkDsoal = Check(gbA, "Restore EAX reverb and 3D positional sound", 12, 24,
                "BG&E was made for hardware 3D audio with EAX, which modern Windows can't do. DSOAL rebuilds it in software with OpenAL Soft.");
            L(gbA, "Output", 330, 24);
            cbSpeakers = Combo(gbA, 384, 22, 190, "Windows default", "Headphones (3D HRTF)", "Stereo speakers", "5.1 surround", "7.1 surround");
            gbA.Controls.Add(cbSpeakers);
            p.Controls.Add(gbA);
            return p;
        }

        TabPage BuildDarkroomTab()
        {
            var p = Page("Darkroom");
            Note(p, "The 13th MDisk is in a locked cabinet on the first floor of the Akuda Bar, next to Francis's puck table. Its code used to come from Ubisoft's Darkroom website, which has shut down. Copy the 16-character Internet code shown in green on the game's Save/Load screen, then decode it here.", 16, 16, 570, 50);
            L(p, "Internet code", 16, 80);
            var tbCode = new TextBox { Location = new Point(130, 78), Width = 260, MaxLength = 32, Font = new Font("Consolas", 12f), CharacterCasing = CharacterCasing.Normal };
            p.Controls.Add(tbCode);
            var btn = MkButton("Decode", new Point(400, 76), 100);
            p.Controls.Add(btn);
            L(p, "Cabinet code", 16, 130);
            var result = new TextBox { Location = new Point(130, 124), Width = 260, ReadOnly = true, Font = new Font("Consolas", 16f, FontStyle.Bold),
                                       BackColor = Color.FromArgb(50, 58, 54), ForeColor = Color.FromArgb(120, 200, 160), BorderStyle = BorderStyle.FixedSingle };
            p.Controls.Add(result);
            var msg = Note(p, "", 16, 172, 570, 60);
            Note(p, "Codes are case-sensitive. The game uses + \\ && / ? ! as some of the characters, and has no I, O, l or 0 - type them exactly as shown.", 16, 240, 570, 36);
            Note(p, "Decoding method reverse-engineered by the BG&&E community (r/beyondgoodandevil, and 9214's daruma).", 16, 290, 570, 20);
            EventHandler go = delegate
            {
                string code = tbCode.Text.Trim().Replace(" ", "");
                string r = Darkroom.Decode(code);
                if (r == null)
                {
                    result.Text = "";
                    msg.Text = code.Length != 16 ? "The code must be exactly 16 characters (you entered " + code.Length + ")."
                                                 : "That code contains a character the game never uses. Check for typos (it has no I, O, l or 0).";
                    return;
                }
                result.Text = r;
                bool odd = r.Any(c => char.IsLetterOrDigit(c) == false);
                msg.Text = odd ? "This reply contains unusual characters, so the Internet code probably has a typo. Double-check it against the game."
                               : "Enter this code on the Akuda Bar cabinet's keypad and press Validate.";
            };
            btn.Click += go;
            tbCode.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; go(s, e); } };
            return p;
        }

        TabPage BuildToolsTab()
        {
            var p = Page("Tools");
            int y = 16;
            L(p, "Game folder:", 16, y);
            var tb = new TextBox { Location = new Point(110, y), Width = 400, Text = GameDir ?? "", ReadOnly = true };
            p.Controls.Add(tb);
            var browse = MkButton("...", new Point(518, y - 3), 60);
            browse.Click += delegate
            {
                using (var f = new FolderBrowserDialog())
                    if (f.ShowDialog() == DialogResult.OK && File.Exists(System.IO.Path.Combine(f.SelectedPath, "BGE.exe")))
                    { GameDir = f.SelectedPath; tb.Text = GameDir; cfg.Set("GameDir", GameDir); cfg.Save("launcher"); }
            };
            p.Controls.Add(browse);
            y += 44;
            var b1 = MkButton("Open game folder", new Point(16, y), 180); b1.Click += delegate { if (GameDir != null) Process.Start("explorer.exe", "\"" + GameDir + "\""); }; p.Controls.Add(b1);
            var b2 = MkButton("Original settings app", new Point(206, y), 180); b2.Click += delegate { Run("SettingsApplication.exe"); }; p.Controls.Add(b2);
            var b3 = MkButton("Open bgehook.log", new Point(396, y), 180); b3.Click += delegate { OpenFile("bgehook.log"); }; p.Controls.Add(b3);
            y += 50;
            var laa = MkButton("Re-apply 4GB patch", new Point(16, y), 180);
            laa.Click += delegate { string m = EnsureLaa(); SetStatus(m); };
            p.Controls.Add(laa);
            var orig = MkButton("Restore original BGE.exe", new Point(206, y), 180);
            orig.Click += delegate { SetStatus(RestoreExe()); };
            p.Controls.Add(orig);
            Note(p, "The 4GB (Large Address Aware) flag lets the 32-bit game use 4 GB, which DXVK needs. A backup of the original exe is kept in the launcher's backup folder.", 396, y - 4, 200, 80);
            y += 90;
            var un = MkButton("Uninstall enhancements", new Point(16, y), 180);
            un.BackColor = Color.FromArgb(110, 50, 50);
            un.Click += delegate { Uninstall(); };
            p.Controls.Add(un);
            Note(p, "Removes bgehook, ReShade and DXVK files from the game folder, restores BGE.exe, and removes the ReShade Vulkan layer registration. The game goes back to stock.", 206, y - 2, 380, 60);
            y += 80;
            Note(p, "Ubisoft Connect may restore BGE.exe when it verifies or updates the game. The launcher re-applies the patch on every Play.", 16, y, 570, 40);
            return p;
        }

        // ------------------------------------------------------------ load / save
        static string FindGame()
        {
            var tries = new List<string>();
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Ubisoft\Beyond Good & Evil"))
                    if (k != null) tries.Add(k.GetValue("Install path") as string);
                // Ubisoft Connect
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs"))
                    if (k != null) foreach (var id in k.GetSubKeyNames()) using (var g = k.OpenSubKey(id)) if (g != null) tries.Add(g.GetValue("InstallDir") as string);
                // GOG
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\GOG.com\Games"))
                    if (k != null) foreach (var id in k.GetSubKeyNames()) using (var g = k.OpenSubKey(id)) if (g != null) tries.Add(g.GetValue("path") as string);
                // Steam, all library folders
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Valve\Steam"))
                {
                    string steam = k == null ? null : k.GetValue("InstallPath") as string;
                    if (steam != null)
                    {
                        var libs = new List<string> { steam };
                        string vdf = System.IO.Path.Combine(steam, @"steamapps\libraryfolders.vdf");
                        if (File.Exists(vdf))
                            foreach (System.Text.RegularExpressions.Match m in System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s*\"([^\"]+)\""))
                                libs.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
                        foreach (var l in libs) tries.Add(System.IO.Path.Combine(l, @"steamapps\common\Beyond Good and Evil"));
                    }
                }
            }
            catch { }
            tries.Add(@"C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Beyond Good and Evil");
            tries.Add(AppDomain.CurrentDomain.BaseDirectory);
            foreach (var t in tries)
            {
                if (string.IsNullOrEmpty(t)) continue;
                string p = t.Replace('/', '\\').TrimEnd('\\');
                try { if (File.Exists(p + "\\BGE.exe")) return p; } catch { }
            }
            return null;
        }

        static List<string> ScreenModes()
        {
            var set = new SortedSet<Tuple<int, int>>();
            var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            for (int i = 0; EnumDisplaySettings(null, i, ref dm); i++)
                if (dm.dmPelsWidth >= 800 && dm.dmBitsPerPel >= 32) set.Add(Tuple.Create(dm.dmPelsWidth, dm.dmPelsHeight));
            var list = set.Reverse().Select(t => t.Item1 + " x " + t.Item2).ToList();
            list.Insert(0, "Desktop resolution");
            return list;
        }

        static int Reg(string name, int def)
        {
            using (var k = Registry.CurrentUser.OpenSubKey(RegVideo))
            {
                object v = k == null ? null : k.GetValue(name);
                return v is int ? (int)v : def;
            }
        }

        void LoadValues()
        {
            if (cfg.S("GameDir", "") != "" && File.Exists(cfg.S("GameDir", "") + "\\BGE.exe")) GameDir = cfg.S("GameDir", "");
            string res = cfg.S("Resolution", "Desktop resolution");
            cbRes.SelectedIndex = Math.Max(0, cbRes.Items.IndexOf(res));
            cbWindow.SelectedIndex = cfg.I("WindowMode", 2);
            cbToggleKey.SelectedIndex = Math.Max(0, cbToggleKey.Items.IndexOf(cfg.S("ToggleKey", "Alt+Enter")));
            int fps = cfg.I("FPS", 60);
            cbFps.SelectedIndex = fps == 30 ? 1 : fps == 120 ? 2 : fps == 144 ? 3 : fps == 0 ? 4 : 0;
            cbVsync.SelectedIndex = cfg.I("VSyncIdx", 0);
            chkWide.Checked = cfg.B("Widescreen", true);
            chkFlipPadX.Checked = cfg.B("FlipStickX", false); chkFlipPadY.Checked = cfg.B("FlipStickY", false);
            chkFlipMouseX.Checked = cfg.B("FlipMouseX", false); chkFlipMouseY.Checked = cfg.B("FlipMouseY", false);
            tbFov.Value = Math.Max(80, Math.Min(130, cfg.I("FOV", 100))); lblFov.Text = tbFov.Value + "%";
            cbHud.SelectedIndex = cfg.I("HUD", 0);

            int an = cfg.I("Aniso", 16);
            cbAniso.SelectedIndex = an >= 16 ? 4 : an >= 8 ? 3 : an >= 4 ? 2 : an >= 2 ? 1 : 0;
            int ms = cfg.I("MSAA", 0);
            cbMsaa.SelectedIndex = ms >= 8 ? 3 : ms >= 4 ? 2 : ms >= 2 ? 1 : 0;
            cbWater.SelectedIndex = Math.Max(0, Math.Min(2, Reg("Water quality", 2)));
            tbShadow.Value = Math.Max(0, Math.Min(255, Reg("Shadows Quality", 128))); lblShadow.Text = tbShadow.Value.ToString();
            cbBuffers.SelectedIndex = Reg("Frame buffers", 3) >= 3 ? 1 : 0;
            chkFlare.Checked = Reg("Flare", 1) != 0;
            chkAfterFx.Checked = Reg("AfterFx", 1) != 0;
            chkBlur.Checked = Reg("Blurryness", 0) != 0;
            chkMip.Checked = Reg("Mipmapping", 1) != 0;
            chkHwVp.Checked = Reg("HardwareVertexProcessing", 0) != 0;

            string rend = cfg.S("Renderer", "dxvk");
            rbNative.Checked = rend == "native"; rbDxvk.Checked = rend != "native";
            chkRcas.Checked = cfg.B("RCAS", true);
            tbSharp.Value = Math.Max(0, Math.Min(20, cfg.I("Sharpen", 16))); lblSharp.Text = (tbSharp.Value * 5) + "%";
            chkSmaa.Checked = cfg.B("SMAA", true);
            chkDeband.Checked = cfg.B("Deband", false);
            chkMxao.Checked = cfg.B("MXAO", true);
            chkGi.Checked = cfg.B("GI", true);
            chkDsoal.Checked = cfg.B("DSOAL", true);
            cbSpeakers.SelectedIndex = Math.Max(0, Math.Min(4, cfg.I("Speakers", 0)));
        }

        void UpdateEnables()
        {
            bool vk = rbDxvk.Checked;
            // Don't disable controls: WinForms draws disabled text dark grey, unreadable on the dark theme.
            var on = Color.WhiteSmoke; var off = Color.FromArgb(150, 162, 156);
            foreach (var c in new[] { chkSmaa, chkMxao, chkGi }) c.ForeColor = vk ? on : off;
            chkSmaa.Text = "SMAA anti-aliasing" + (vk ? "" : " (Vulkan only)");
            chkMxao.Text = "MXAO ambient occlusion" + (vk ? "" : " (Vulkan only)");
            chkGi.Text = "Global illumination - NiceGuy SSGI" + (vk ? "" : " (Vulkan only)");
            UpdateImmerseLabel();
        }

        bool HaveImmerse()
        {
            return GameDir != null && File.Exists(System.IO.Path.Combine(GameDir, @"reshade-shaders\Shaders\MartysMods_MXAO.fx"));
        }
        void UpdateImmerseLabel()
        {
            if (lblImmerse != null) lblImmerse.Text = HaveImmerse() ? "Installed in the game folder." : "Not downloaded yet.";
        }

        // iMMERSE's licence forbids redistribution, so fetch it from the author's own GitHub.
        bool DownloadImmerse()
        {
            const string url = "https://github.com/martymcmodding/iMMERSE/archive/refs/heads/main.zip";
            string tmp = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "BGE-iMMERSE.zip");
            try
            {
                SetStatus("Downloading iMMERSE shaders from GitHub..."); Application.DoEvents();
                System.Net.ServicePointManager.SecurityProtocol |= (System.Net.SecurityProtocolType)3072; // TLS 1.2
                using (var wc = new System.Net.WebClient()) wc.DownloadFile(url, tmp);
                string dst = System.IO.Path.GetFullPath(System.IO.Path.Combine(GameDir, "reshade-shaders"));
                using (var zip = System.IO.Compression.ZipFile.OpenRead(tmp))
                    foreach (var e in zip.Entries)
                    {
                        int slash = e.FullName.IndexOf('/');
                        if (slash < 0 || e.Name.Length == 0) continue;
                        string rel = e.FullName.Substring(slash + 1);
                        if (!rel.StartsWith("Shaders/") && !rel.StartsWith("Textures/")) continue;
                        string outp = System.IO.Path.GetFullPath(System.IO.Path.Combine(dst, rel.Replace('/', '\\')));
                        if (!outp.StartsWith(dst + "\\", StringComparison.OrdinalIgnoreCase)) continue;
                        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(outp));
                        System.IO.Compression.ZipFileExtensions.ExtractToFile(e, outp, true);
                    }
                File.Delete(tmp);
                return HaveImmerse();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Couldn't download the iMMERSE shaders:\n" + ex.Message +
                    "\n\nYou can get them from github.com/martymcmodding/iMMERSE and copy its Shaders and Textures folders into the game's reshade-shaders folder.", "BGE Launcher");
                return false;
            }
        }

        void SaveLauncherCfg()
        {
            cfg.Set("GameDir", GameDir ?? "");
            cfg.Set("Resolution", cbRes.SelectedItem);
            cfg.Set("WindowMode", cbWindow.SelectedIndex);
            cfg.Set("ToggleKey", cbToggleKey.SelectedItem);
            cfg.Set("FPS", new[] { 60, 30, 120, 144, 0 }[cbFps.SelectedIndex]);
            cfg.Set("VSyncIdx", cbVsync.SelectedIndex);
            cfg.Set("Widescreen", chkWide.Checked);
            cfg.Set("FlipStickX", chkFlipPadX.Checked); cfg.Set("FlipStickY", chkFlipPadY.Checked);
            cfg.Set("FlipMouseX", chkFlipMouseX.Checked); cfg.Set("FlipMouseY", chkFlipMouseY.Checked);
            cfg.Set("FOV", tbFov.Value);
            cfg.Set("HUD", cbHud.SelectedIndex);
            cfg.Set("Aniso", new[] { 0, 2, 4, 8, 16 }[cbAniso.SelectedIndex]);
            cfg.Set("MSAA", new[] { 0, 2, 4, 8 }[cbMsaa.SelectedIndex]);
            cfg.Set("Renderer", rbDxvk.Checked ? "dxvk" : "native");
            cfg.Set("RCAS", chkRcas.Checked); cfg.Set("Sharpen", tbSharp.Value);
            cfg.Set("SMAA", chkSmaa.Checked); cfg.Set("Deband", chkDeband.Checked);
            cfg.Set("MXAO", chkMxao.Checked); cfg.Set("GI", chkGi.Checked);
            cfg.Set("DSOAL", chkDsoal.Checked); cfg.Set("Speakers", cbSpeakers.SelectedIndex);
            cfg.Save("launcher");
        }

        void WriteRegistry()
        {
            using (var k = Registry.CurrentUser.CreateSubKey(RegVideo))
            {
                k.SetValue("Water quality", cbWater.SelectedIndex, RegistryValueKind.DWord);
                k.SetValue("Shadows Quality", tbShadow.Value, RegistryValueKind.DWord);
                k.SetValue("Frame buffers", cbBuffers.SelectedIndex == 1 ? 3 : 2, RegistryValueKind.DWord);
                k.SetValue("Flare", chkFlare.Checked ? 1 : 0, RegistryValueKind.DWord);
                k.SetValue("AfterFx", chkAfterFx.Checked ? 1 : 0, RegistryValueKind.DWord);
                k.SetValue("Blurryness", chkBlur.Checked ? 1 : 0, RegistryValueKind.DWord);
                k.SetValue("Mipmapping", chkMip.Checked ? 1 : 0, RegistryValueKind.DWord);
                k.SetValue("HardwareVertexProcessing", chkHwVp.Checked ? 1 : 0, RegistryValueKind.DWord);
            }
            using (var k = Registry.CurrentUser.CreateSubKey(RegGame + @"\SettingsApplication.INI\Sound"))
            {
                // BGE.exe turns EAX on only when "Sound FX" != 1 and "Audio board FX Capable" == 2;
                // it then loads eax.dll (ours, deployed with DSOAL) and must find it. With DSOAL off,
                // put both back to 1 so the game never looks for eax.dll.
                k.SetValue("Sound FX", chkDsoal.Checked ? 0 : 1, RegistryValueKind.DWord);
                k.SetValue("Audio board FX Capable", chkDsoal.Checked ? 2 : 1, RegistryValueKind.DWord);
            }
        }

        // ------------------------------------------------------------ deploy
        bool CopyIfChanged(string src, string dst)
        {
            var s = new FileInfo(src);
            if (!s.Exists) return false;
            var d = new FileInfo(dst);
            if (d.Exists && d.Length == s.Length && d.LastWriteTimeUtc >= s.LastWriteTimeUtc) return false;
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(dst));
            File.Copy(src, dst, true);
            return true;
        }
        void CopyDir(string src, string dst)
        {
            if (!Directory.Exists(src)) return;
            foreach (var f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
                CopyIfChanged(f, System.IO.Path.Combine(dst, f.Substring(src.Length + 1)));
        }

        void Deploy(string renderer, bool wantReshade)
        {
            CopyIfChanged(System.IO.Path.Combine(Payload, "hook\\d3d9.dll"), System.IO.Path.Combine(GameDir, "d3d9.dll"));
            if (wantReshade)
            {
                CopyIfChanged(System.IO.Path.Combine(Payload, "ReShade32.dll"), System.IO.Path.Combine(GameDir, "ReShade32.dll"));
                CopyDir(System.IO.Path.Combine(Payload, "reshade-shaders"), System.IO.Path.Combine(GameDir, "reshade-shaders"));
            }
            if (renderer == "dxvk")
                CopyIfChanged(System.IO.Path.Combine(Payload, "dxvk_d3d9.dll"), System.IO.Path.Combine(GameDir, "dxvk_d3d9.dll"));
            DeployAudio();
        }

        // DSOAL: a dsound.dll that implements DirectSound3D + EAX on top of OpenAL Soft.
        void DeployAudio()
        {
            string src = System.IO.Path.Combine(Payload, "dsoal");
            string dsound = System.IO.Path.Combine(GameDir, "dsound.dll");
            if (chkDsoal.Checked)
            {
                CopyIfChanged(System.IO.Path.Combine(src, "dsound.dll"), dsound);
                CopyIfChanged(System.IO.Path.Combine(src, "dsoal-aldrv.dll"), System.IO.Path.Combine(GameDir, "dsoal-aldrv.dll"));
                CopyIfChanged(System.IO.Path.Combine(src, "eax.dll"), System.IO.Path.Combine(GameDir, "eax.dll"));
                string nl = "\r\n";
                string[] modes = {
                    "",
                    "channels=stereo" + nl + "stereo-mode=headphones" + nl + "stereo-encoding=hrtf" + nl,
                    "channels=stereo" + nl + "stereo-mode=speakers" + nl,
                    "channels=surround51" + nl,
                    "channels=surround71" + nl };
                File.WriteAllText(System.IO.Path.Combine(GameDir, "alsoft.ini"),
                    "# written by BGE launcher (OpenAL Soft settings for DSOAL)" + nl + "[general]" + nl + modes[cbSpeakers.SelectedIndex]);
            }
            else RemoveAudio();
        }

        void RemoveAudio()
        {
            // only remove a dsound.dll that is ours, never someone else's audio wrapper (e.g. Creative ALchemy)
            var mine = new FileInfo(System.IO.Path.Combine(Payload, @"dsoal\dsound.dll"));
            var cur = new FileInfo(System.IO.Path.Combine(GameDir, "dsound.dll"));
            if (!(cur.Exists && mine.Exists && cur.Length == mine.Length)) return;
            foreach (var f in new[] { "dsound.dll", "dsoal-aldrv.dll", "eax.dll", "alsoft.ini", "dsoal.log", "alsoft.log", "eax.log" })
            { string pth = System.IO.Path.Combine(GameDir, f); if (File.Exists(pth)) File.Delete(pth); }
            // without eax.dll the game must not try EAX (it would call a null pointer)
            using (var k = Registry.CurrentUser.CreateSubKey(RegGame + @"\SettingsApplication.INI\Sound"))
            {
                k.SetValue("Sound FX", 1, RegistryValueKind.DWord);
                k.SetValue("Audio board FX Capable", 1, RegistryValueKind.DWord);
            }
        }

        void RegisterVkLayer(bool on)
        {
            string json = System.IO.Path.Combine(Payload, "ReShade32_BGE.json");
            using (var k = Registry.CurrentUser.CreateSubKey(@"SOFTWARE\Khronos\Vulkan\ImplicitLayers"))
            {
                if (on)
                {
                    // manifest points at the game's ReShade32.dll; rewrite in case the game moved
                    string dll = System.IO.Path.Combine(GameDir, "ReShade32.dll").Replace("\\", "\\\\");
                    string text = File.ReadAllText(json);
                    text = System.Text.RegularExpressions.Regex.Replace(text, "\"library_path\":\\s*\"[^\"]*\"", "\"library_path\": \"" + dll + "\"");
                    File.WriteAllText(json, text);
                    k.SetValue(json, 0, RegistryValueKind.DWord);
                }
                else if (k.GetValue(json) != null) k.DeleteValue(json);
            }
        }

        void WriteReshade(string renderer, bool immerse)
        {
            var techs = new List<string>();
            bool vk = renderer == "dxvk" && immerse;
            if (vk && chkGi.Checked) techs.Add("MartysMods_Launchpad@MartysMods_LAUNCHPAD.fx");
            if (vk && chkMxao.Checked) techs.Add("MartysMods_MXAO@MartysMods_MXAO.fx");
            if (vk && chkGi.Checked) techs.Add("NGLighting@NGLighting.fx");
            if (chkDeband.Checked) techs.Add("Deband@Deband.fx");
            if (vk && chkSmaa.Checked) { techs.Add("MartysMods_AntiAliasing_Prepass@MartysMods_SMAA.fx"); techs.Add("MartysMods_AntiAliasing@MartysMods_SMAA.fx"); }
            if (chkRcas.Checked) techs.Add("FSR_RCAS@BGE_FSR_RCAS.fx");
            string list = string.Join(",", techs);
            // sharpness slider: 100% -> 0 stops (max), 0% -> 2 stops (subtle)
            double stops = 2.0 - tbSharp.Value / 10.0;
            File.WriteAllText(System.IO.Path.Combine(GameDir, "BGE-ReShadePreset.ini"),
                "PreprocessorDefinitions=MV_METHOD=1\r\nTechniques=" + list + "\r\nTechniqueSorting=" + list + "\r\n\r\n[BGE_FSR_RCAS.fx]\r\nSharpness=" +
                stops.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) + "\r\n");
            string ini = System.IO.Path.Combine(GameDir, "ReShade.ini");
            if (!File.Exists(ini))
                File.WriteAllText(ini,
                    "[GENERAL]\r\nEffectSearchPaths=.\\reshade-shaders\\Shaders\\**\r\nTextureSearchPaths=.\\reshade-shaders\\Textures\\**\r\n" +
                    "PresetPath=.\\BGE-ReShadePreset.ini\r\nPerformanceMode=1\r\n" +
                    "PreprocessorDefinitions=RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,RESHADE_DEPTH_INPUT_IS_REVERSED=0,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0\r\n" +
                    "[INPUT]\r\nKeyOverlay=36,0,0,0\r\n[OVERLAY]\r\nTutorialProgress=4\r\n[DEPTH]\r\nDepthCopyBeforeClears=1\r\n");
        }

        void WriteHook(string renderer, bool reshade)
        {
            var h = new Ini(System.IO.Path.Combine(GameDir, "bgehook.ini"));
            h.Set("FPSLimit", new[] { 60, 30, 120, 144, 0 }[cbFps.SelectedIndex]);
            h.Set("VSync", new[] { 0, 1, -1 }[cbVsync.SelectedIndex]);
            int w = 0, ht = 0;
            if (cbRes.SelectedIndex > 0)
            {
                var parts = cbRes.SelectedItem.ToString().Split('x');
                w = int.Parse(parts[0].Trim()); ht = int.Parse(parts[1].Trim());
            }
            else { w = Screen.PrimaryScreen.Bounds.Width; ht = Screen.PrimaryScreen.Bounds.Height; }
            h.Set("Width", w); h.Set("Height", ht);
            h.Set("WindowMode", cbWindow.SelectedIndex == 0 ? 0 : cbWindow.SelectedIndex == 1 ? 1 : 2);
            h.Set("ToggleKey", cbToggleKey.SelectedItem);
            h.Set("Anisotropy", new[] { 0, 2, 4, 8, 16 }[cbAniso.SelectedIndex]);
            h.Set("MSAA", renderer == "native" ? new[] { 0, 2, 4, 8 }[cbMsaa.SelectedIndex] : 0);
            h.Set("WidescreenFix", chkWide.Checked);
            h.Set("FOVPercent", tbFov.Value);
            h.Set("HUDMode", cbHud.SelectedIndex);
            h.Set("FlipStickX", chkFlipPadX.Checked); h.Set("FlipStickY", chkFlipPadY.Checked);
            h.Set("FlipMouseX", chkFlipMouseX.Checked); h.Set("FlipMouseY", chkFlipMouseY.Checked);
            h.Set("Chain", renderer == "dxvk" ? "dxvk_d3d9.dll" : (reshade ? "ReShade32.dll" : ""));
            h.Set("ReShadeVulkan", renderer == "dxvk" && reshade);
            h.Set("Log", 1);
            h.Save("bgehook");

            if (renderer == "dxvk")
            {
                // our limiter does the pacing; disable DXVK's built-in BGE 60 fps profile so they don't fight
                File.WriteAllText(System.IO.Path.Combine(GameDir, "dxvk.conf"),
                    "# written by BGE launcher\r\nd3d9.maxFrameRate = 0\r\nd3d9.samplerAnisotropy = " + new[] { -1, 2, 4, 8, 16 }[cbAniso.SelectedIndex] + "\r\n");
            }
        }

        bool ApplyAll()
        {
            if (GameDir == null) { MessageBox.Show(this, "Game folder not found."); return false; }
            if (!File.Exists(System.IO.Path.Combine(Payload, @"hook\d3d9.dll")))
            {
                MessageBox.Show(this, "The launcher's \"payload\" folder is missing next to BGELauncher.exe:\n" + Payload +
                    "\n\nRun BGELauncher.exe from the folder you extracted the release zip into, and keep the payload folder with it.", "BGE Launcher");
                return false;
            }
            if (Process.GetProcessesByName("BGE").Length > 0) { MessageBox.Show(this, "Close the game first."); return false; }
            try
            {
                string renderer = rbDxvk.Checked ? "dxvk" : "native";
                bool wantImmerse = renderer == "dxvk" && (chkSmaa.Checked || chkMxao.Checked || chkGi.Checked);
                bool reshade = chkRcas.Checked || chkDeband.Checked || wantImmerse;
                SaveLauncherCfg();
                Deploy(renderer, reshade);
                if (wantImmerse && !HaveImmerse())
                {
                    var ans = MessageBox.Show(this, "SMAA, MXAO and GI use the free iMMERSE shaders by Pascal \"Marty McFly\" Gilcher, which can't be bundled with this launcher.\n\n" +
                        "Download them now from github.com/martymcmodding/iMMERSE?\n\n(No = play without SMAA, MXAO and GI this time.)", "BGE Launcher", MessageBoxButtons.YesNo);
                    if (ans != DialogResult.Yes || !DownloadImmerse()) { wantImmerse = false; reshade = chkRcas.Checked || chkDeband.Checked; }
                    UpdateImmerseLabel();
                }
                WriteRegistry();
                WriteHook(renderer, reshade);
                if (reshade) WriteReshade(renderer, wantImmerse);
                RegisterVkLayer(renderer == "dxvk" && reshade);
                if (renderer != "native") { string m = EnsureLaa(); if (m.StartsWith("Error")) SetStatus(m); }
                return true;
            }
            catch (UnauthorizedAccessException)
            {
                MessageBox.Show(this, "No write access to the game folder. Run the launcher as administrator once.", "BGE Launcher");
                return false;
            }
            catch (Exception ex) { MessageBox.Show(this, ex.ToString(), "BGE Launcher"); return false; }
        }

        void Play()
        {
            if (!ApplyAll()) return;
            Run("BGE.exe");
            SetStatus("Launched. If Ubisoft Connect opens first, the game starts right after it.");
        }

        void Run(string exe)
        {
            if (GameDir == null) return;
            var psi = new ProcessStartInfo(System.IO.Path.Combine(GameDir, exe)) { WorkingDirectory = GameDir, UseShellExecute = true };
            try { Process.Start(psi); } catch (Exception ex) { MessageBox.Show(this, ex.Message); }
        }
        void OpenFile(string name)
        {
            string p = GameDir == null ? null : System.IO.Path.Combine(GameDir, name);
            if (p != null && File.Exists(p)) Process.Start("notepad.exe", "\"" + p + "\"");
        }

        // ------------------------------------------------------------ exe patch
        string BackupExe { get { return System.IO.Path.Combine(Root, "backup\\BGE.exe.original"); } }

        string EnsureLaa()
        {
            try
            {
                string exe = System.IO.Path.Combine(GameDir, "BGE.exe");
                byte[] b = File.ReadAllBytes(exe);
                int pe = BitConverter.ToInt32(b, 0x3C);
                int off = pe + 4 + 18;
                ushort ch = BitConverter.ToUInt16(b, off);
                if ((ch & 0x20) != 0) return "4GB patch already applied.";
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(BackupExe));
                File.Copy(exe, BackupExe, true); // newest original (Ubisoft may have updated it)
                ch |= 0x20;
                b[off] = (byte)(ch & 0xFF); b[off + 1] = (byte)(ch >> 8);
                File.WriteAllBytes(exe, b);
                return "4GB patch applied (original backed up).";
            }
            catch (Exception ex) { return "Error applying 4GB patch: " + ex.Message; }
        }

        string RestoreExe()
        {
            if (!File.Exists(BackupExe)) return "No backup found - BGE.exe was never patched.";
            if (Process.GetProcessesByName("BGE").Length > 0) return "Close the game first.";
            File.Copy(BackupExe, System.IO.Path.Combine(GameDir, "BGE.exe"), true);
            return "Original BGE.exe restored.";
        }

        void Uninstall()
        {
            if (GameDir == null) return;
            if (MessageBox.Show(this, "Remove all launcher files from the game folder and restore the stock game?", "Uninstall enhancements", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
            if (Process.GetProcessesByName("BGE").Length > 0) { MessageBox.Show(this, "Close the game first."); return; }
            foreach (var f in new[] { "d3d9.dll", "bgehook.ini", "bgehook.log", "ReShade32.dll", "ReShade.ini", "ReShade.log", "BGE-ReShadePreset.ini",
                                      "dxvk_d3d9.dll", "dxvk.conf", "BGE_d3d9.log", "BGE.dxvk-cache" })
            {
                string p = System.IO.Path.Combine(GameDir, f);
                if (File.Exists(p)) File.Delete(p);
            }
            foreach (var d in new[] { "reshade-shaders" })
            {
                string p = System.IO.Path.Combine(GameDir, d);
                if (Directory.Exists(p)) Directory.Delete(p, true);
            }
            RemoveAudio();
            RegisterVkLayer(false);
            SetStatus(RestoreExe() + " Enhancements removed.");
        }

        // ------------------------------------------------------------ interop
        [DllImport("user32.dll", CharSet = CharSet.Ansi)]
        static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        struct DEVMODE
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
        }
    }
}
