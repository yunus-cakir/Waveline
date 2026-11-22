using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;

namespace Waveline.Settings;

/// <summary>
/// Edits %APPDATA%\Waveline\clip.json. The engine (WavelineClip.exe) watches that file and
/// applies changes within a second, so there is no IPC: saving the file is the "apply".
/// Device lists come from devices.json, written by the engine.
/// </summary>
public partial class MainWindow : Window
{
    private sealed record Choice(string Value, string Label);

    private static readonly string AppData =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Waveline");
    private static readonly string ConfigPath = Path.Combine(AppData, "clip.json");
    private static readonly string DevicesPath = Path.Combine(AppData, "devices.json");
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Waveline", "clip.log");

    private JsonObject _config = new();

    public MainWindow()
    {
        InitializeComponent();
        Load();
    }

    // ---------------------------------------------------------------- load

    private void Load()
    {
        try
        {
            if (File.Exists(ConfigPath))
                _config = JsonNode.Parse(File.ReadAllText(ConfigPath)) as JsonObject ?? new JsonObject();
        }
        catch (Exception ex)
        {
            StatusText.Text = "Ayar dosyası okunamadı: " + ex.Message;
            _config = new JsonObject();
        }

        JsonObject devices = new();
        try
        {
            if (File.Exists(DevicesPath))
                devices = JsonNode.Parse(File.ReadAllText(DevicesPath)) as JsonObject ?? new JsonObject();
        }
        catch { /* engine not started yet: lists stay minimal */ }

        MinutesSlider.Value = Math.Round(Get("minutes", 1.0) * 4) / 4;  // snap old values to 15 s
        DirBox.Text = Get("dir", "");
        ClipHotkeyBox.Text = Get("clipHotkey", "Ctrl+Alt+F10");
        RecordHotkeyBox.Text = Get("recordHotkey", "Ctrl+Alt+F9");
        SelectByTag(CodecBox, Get("codec", "hevc"));
        SelectByContent(FpsBox, Get("fps", 60.0).ToString("0"));
        BitrateSlider.Value = Math.Round(Get("bitrateKbps", 15000.0) / 1000.0);
        CursorBox.IsChecked = Get("cursor", true);
        var split = Get("splitMinutes", 0.0);
        SplitBox.IsChecked = split > 0;
        SplitMinutesBox.Text = (split > 0 ? split : 30).ToString("0.##");
        SplitMinutesBox.IsEnabled = split > 0;
        DesktopBox.IsChecked = Get("desktop", false);
        NotificationsBox.IsChecked = Get("notifications", true);
        SoundBox.IsChecked = Get("sound", true);
        AutostartBox.IsChecked = Get("autostart", false);
        IncludeBox.Text = string.Join(Environment.NewLine, GetList("include"));
        ExcludeBox.Text = string.Join(Environment.NewLine, GetList("exclude"));

        // monitors
        var monitors = new List<Choice> { new("", "Ana monitör (otomatik)") };
        foreach (var m in devices["monitors"]?.AsArray() ?? new JsonArray())
        {
            if (m is not JsonObject o) continue;
            var label = $"{o["device"]}  {o["width"]}×{o["height"]}  {o["adapter"]}";
            if (o["primary"]?.GetValue<bool>() == true) label += "  (ana)";
            if (o["virtual"]?.GetValue<bool>() == true) label += "  (sanal)";
            monitors.Add(new Choice(o["device"]?.GetValue<string>() ?? "", label));
        }
        MonitorBox.ItemsSource = monitors;
        MonitorBox.SelectedItem = monitors.FirstOrDefault(c => c.Value == Get("monitorDevice", "")) ?? monitors[0];

        // notification output devices
        var autoName = devices["autoNotifyDevice"]?.GetValue<string>();
        var outputs = new List<Choice> { new("auto", "Otomatik" + (string.IsNullOrEmpty(autoName) ? "" : $" ({autoName})")) };
        foreach (var d in devices["outputs"]?.AsArray() ?? new JsonArray())
            if (d is JsonObject o)
                outputs.Add(new Choice(o["id"]?.GetValue<string>() ?? "", o["name"]?.GetValue<string>() ?? "?"));
        NotifyDeviceBox.ItemsSource = outputs;
        NotifyDeviceBox.SelectedItem = outputs.FirstOrDefault(c => c.Value == Get("notifyDevice", "auto")) ?? outputs[0];

        var defaults = devices["defaultExcludes"]?.AsArray().Select(n => n?.GetValue<string>()).Where(s => s != null);
        if (defaults != null)
            DefaultExcludesText.Text = "Zaten hariç: " + string.Join(", ", defaults);

        UpdateMinutesText();
        UpdateBitrateText();
    }

    private T Get<T>(string key, T fallback)
    {
        try
        {
            var node = _config[key];
            return node is null ? fallback : node.GetValue<T>();
        }
        catch { return fallback; }
    }

    private IEnumerable<string> GetList(string key) =>
        (_config[key] as JsonArray)?.Select(n => n?.GetValue<string>() ?? "").Where(s => s.Length > 0) ?? Enumerable.Empty<string>();

    private static void SelectByTag(ComboBox box, string tag) =>
        box.SelectedItem = box.Items.OfType<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == tag) ?? box.Items[0];

    private static void SelectByContent(ComboBox box, string content) =>
        box.SelectedItem = box.Items.OfType<ComboBoxItem>().FirstOrDefault(i => (string)i.Content == content) ?? box.Items[1];

    // ---------------------------------------------------------------- live labels

    private void MinutesSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => UpdateMinutesText();
    private void BitrateSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { UpdateBitrateText(); UpdateMinutesText(); }

    private void UpdateMinutesText()
    {
        if (MinutesText is null || MemoryHint is null || BitrateSlider is null) return;
        var minutes = MinutesSlider.Value;
        MinutesText.Text = FormatDuration((int)Math.Round(minutes * 60));
        // video bitrate + 5 AAC tracks at 192 kbps
        var mb = (BitrateSlider.Value * 1000 + 5 * 192) / 8.0 / 1024.0 * minutes * 60.0;
        MemoryHint.Text = $"Tahmini RAM kullanımı: en fazla ~{mb:0} MB (sahne hareketsizken çok daha az).";
    }

    // 45 -> "45 sn", 60 -> "1 dk", 75 -> "1 dk 15 sn"
    private static string FormatDuration(int seconds)
    {
        int m = seconds / 60, s = seconds % 60;
        if (m == 0) return $"{s} sn";
        return s == 0 ? $"{m} dk" : $"{m} dk {s} sn";
    }

    private void UpdateBitrateText()
    {
        if (BitrateText is null) return;
        BitrateText.Text = $"{BitrateSlider.Value:0} Mbps";
    }

    private void SplitBox_Changed(object sender, RoutedEventArgs e)
    {
        if (SplitMinutesBox != null) SplitMinutesBox.IsEnabled = SplitBox.IsChecked == true;
    }

    // ---------------------------------------------------------------- hotkey capture

    private void HotkeyBox_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        e.Handled = true;
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift or Key.RightShift
            or Key.LWin or Key.RWin or Key.Tab)
            return;
        var name = KeyName(key);
        if (name is null) return;

        var parts = new List<string>();
        var mods = Keyboard.Modifiers;
        if (mods.HasFlag(ModifierKeys.Control)) parts.Add("Ctrl");
        if (mods.HasFlag(ModifierKeys.Alt)) parts.Add("Alt");
        if (mods.HasFlag(ModifierKeys.Shift)) parts.Add("Shift");
        if (mods.HasFlag(ModifierKeys.Windows)) parts.Add("Win");
        if (parts.Count == 0)
        {
            StatusText.Text = "En az bir Ctrl/Alt/Shift/Win tuşu gerekli.";
            return;
        }
        parts.Add(name);
        ((TextBox)sender).Text = string.Join("+", parts);
        StatusText.Text = "";
    }

    // Names understood by the engine's parser (tray.cpp ParseHotkey).
    private static string? KeyName(Key key) => key switch
    {
        >= Key.F1 and <= Key.F24 => "F" + (key - Key.F1 + 1),
        >= Key.A and <= Key.Z => key.ToString(),
        >= Key.D0 and <= Key.D9 => ((char)('0' + (key - Key.D0))).ToString(),
        >= Key.NumPad0 and <= Key.NumPad9 => "Num" + (key - Key.NumPad0),
        Key.Snapshot => "PrintScreen",
        Key.Pause => "Pause",
        Key.Insert => "Insert",
        Key.Home => "Home",
        Key.End => "End",
        _ => null,
    };

    // ---------------------------------------------------------------- buttons

    private void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Klip klasörü" };
        if (Directory.Exists(DirBox.Text)) dialog.InitialDirectory = DirBox.Text;
        if (dialog.ShowDialog(this) == true) DirBox.Text = dialog.FolderName;
    }

    private void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        var dir = string.IsNullOrWhiteSpace(DirBox.Text)
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "Waveline")
            : DirBox.Text;
        Directory.CreateDirectory(dir);
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true });
    }

    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (File.Exists(LogPath)) Process.Start(new ProcessStartInfo("notepad.exe", $"\"{LogPath}\"") { UseShellExecute = true });
        else StatusText.Text = "Henüz log yok.";
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        if (ClipHotkeyBox.Text == RecordHotkeyBox.Text)
        {
            StatusText.Text = "İki kısayol aynı olamaz.";
            return;
        }
        double split = 0;
        if (SplitBox.IsChecked == true &&
            (!double.TryParse(SplitMinutesBox.Text.Replace(',', '.'), System.Globalization.NumberStyles.Float,
                              System.Globalization.CultureInfo.InvariantCulture, out split) || split < 1))
        {
            StatusText.Text = "Bölme süresi en az 1 dakika olmalı.";
            return;
        }

        _config["minutes"] = MinutesSlider.Value;
        _config["dir"] = DirBox.Text.Trim();
        _config["clipHotkey"] = ClipHotkeyBox.Text;
        _config["recordHotkey"] = RecordHotkeyBox.Text;
        _config["codec"] = (string)((ComboBoxItem)CodecBox.SelectedItem).Tag;
        _config["fps"] = int.Parse((string)((ComboBoxItem)FpsBox.SelectedItem).Content);
        _config["bitrateKbps"] = (int)(BitrateSlider.Value * 1000);
        _config["cursor"] = CursorBox.IsChecked == true;
        _config["splitMinutes"] = split;
        _config["desktop"] = DesktopBox.IsChecked == true;
        _config["monitorDevice"] = (MonitorBox.SelectedItem as Choice)?.Value ?? "";
        _config["notifications"] = NotificationsBox.IsChecked == true;
        _config["sound"] = SoundBox.IsChecked == true;
        _config["notifyDevice"] = (NotifyDeviceBox.SelectedItem as Choice)?.Value ?? "auto";
        _config["autostart"] = AutostartBox.IsChecked == true;
        _config["include"] = ToArray(IncludeBox.Text);
        _config["exclude"] = ToArray(ExcludeBox.Text);

        try
        {
            Directory.CreateDirectory(AppData);
            var json = _config.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(ConfigPath, json, new UTF8Encoding(false));
            StatusText.Text = Process.GetProcessesByName("WavelineClip").Length > 0
                ? "Kaydedildi, uygulandı."
                : "Kaydedildi (Waveline Clip çalışmıyor).";
        }
        catch (Exception ex)
        {
            StatusText.Text = "Kaydedilemedi: " + ex.Message;
        }
    }

    private static JsonArray ToArray(string text)
    {
        var array = new JsonArray();
        foreach (var line in text.Split('\n', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            var name = line.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? line[..^4] : line;
            if (name.Length > 0) array.Add(name);
        }
        return array;
    }
}
