using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media;
using Microsoft.Win32;

namespace JinraRecomp;

public partial class MainWindow : Window
{
    private const string NvidiaLicense = "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt";
    private const string AmdLicense = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/docs/license.md";
    private readonly Builder builder = new();
    private readonly Queue<string> pendingLog = new();
    private readonly System.Windows.Threading.DispatcherTimer logTimer;
    private CancellationTokenSource? running;
    private Process? game;
    private StreamWriter? log;
    private bool closeAfterOperation;
    private string? saveToImport;
    private string Preferences => Path.Combine(builder.Home, "preferencias.txt");

    public MainWindow(string? source)
    {
        InitializeComponent();
        VersionLabel.Text = $"Versão {Builder.Version}";
        HardwareInfo.Text = BuildProfile.Detect().Description;
        logTimer = new(TimeSpan.FromMilliseconds(240), System.Windows.Threading.DispatcherPriority.Background,
            (_, _) => FlushLog(), Dispatcher);
        logTimer.Start();
        AllowDrop = true;
        DragOver += (_, e) => e.Effects = running is null && e.Data.GetDataPresent(DataFormats.FileDrop)
            ? DragDropEffects.Copy : DragDropEffects.None;
        Drop += (_, e) => { if (running is null && e.Data.GetData(DataFormats.FileDrop) is string[] paths && paths.Length == 1) SelectSource(paths[0]); };
        DestinationBox.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games", "Jinra-Recomp");
        try
        {
            if (File.Exists(Preferences))
            {
                var prefs = File.ReadAllLines(Preferences);
                if (prefs.Length > 0 && Path.IsPathFullyQualified(prefs[0])) DestinationBox.Text = prefs[0];
                if (source is null && prefs.Length > 1 && (File.Exists(prefs[1]) || Directory.Exists(prefs[1]))) source = prefs[1];
            }
            if (File.Exists(builder.BuildLog)) LogBox.Text = string.Join(Environment.NewLine, File.ReadLines(builder.BuildLog).TakeLast(120));
        }
        catch (IOException) { }
        EnableFsr4.IsChecked = Builder.HasFsr4(DestinationBox.Text);
        EnableDlss.IsChecked = Builder.HasDlss(DestinationBox.Text);
        SelectSource(source ?? FindSource());
        ShowBuild(this, new RoutedEventArgs());
        RefreshButtons();
    }

    private static string? FindSource()
    {
        var personal = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        foreach (var directory in new[] { Path.Combine(personal, "Switch Games"), personal,
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"),
            Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory) })
        {
            if (!Directory.Exists(directory)) continue;
            try
            {
                var candidate = Directory.EnumerateFiles(directory)
                    .FirstOrDefault(path => (Path.GetExtension(path).Equals(".nx", StringComparison.OrdinalIgnoreCase) ||
                        Path.GetExtension(path).Equals(".nup", StringComparison.OrdinalIgnoreCase)) &&
                        (Path.GetFileName(path).Contains("0400486024dec000", StringComparison.OrdinalIgnoreCase) ||
                         Path.GetFileName(path).Contains("Ocarina", StringComparison.OrdinalIgnoreCase)));
                if (candidate is not null) return candidate;
            }
            catch (UnauthorizedAccessException) { }
            catch (IOException) { }
        }
        return null;
    }

    private void SelectSource(string? source)
    {
        if (source is null) return;
        SourceBox.Text = source;
        Status.Text = "Cópia selecionada";
        SourceHint.Text = "Verifica a versão ou inicia a compilação.";
    }

    private void ChooseFile(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFileDialog { Title = "Escolher a cópia do Zelda", Filter = "Cópia Switch 2|*.nx;*.nup|Todos os ficheiros|*.*" };
        if (picker.ShowDialog(this) == true) SelectSource(picker.FileName);
    }

    private void ChooseFolder(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFolderDialog { Title = "Escolher pasta com exefs e romfs" };
        if (picker.ShowDialog(this) == true) SelectSource(picker.FolderName);
    }

    private void ChooseDestination(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFolderDialog { Title = "Escolher o destino da compilação" };
        if (picker.ShowDialog(this) != true) return;
        DestinationBox.Text = picker.FolderName;
        EnableFsr4.IsChecked = Builder.HasFsr4(DestinationBox.Text);
        EnableDlss.IsChecked = Builder.HasDlss(DestinationBox.Text);
        RefreshButtons();
    }

    private void SourceChanged(object sender, RoutedEventArgs e) { if (BuildButton is not null) RefreshButtons(); }

    private void RefreshButtons()
    {
        var idle = running is null;
        var playing = game is { HasExited: false };
        FileButton.IsEnabled = FolderButton.IsEnabled = DestinationButton.IsEnabled = WarmShaders.IsEnabled = EnableDlss.IsEnabled = EnableFsr4.IsEnabled = idle;
        CheckButton.IsEnabled = idle && SourceBox.Text.Length > 0;
        var updating = Builder.CanUpdate(DestinationBox.Text);
        BuildButton.Content = updating ? "Atualizar jogo" : "Compilar jogo";
        BuildButton.IsEnabled = idle && !playing && (updating || SourceBox.Text.Length > 0);
        PlayButton.IsEnabled = idle && !playing && DestinationBox.Text.Length > 0 && Builder.Ready(DestinationBox.Text);
        ImportButton.IsEnabled = idle && !playing;
        UpdateButton.IsEnabled = idle && !playing;
        CancelButton.Visibility = idle ? Visibility.Collapsed : Visibility.Visible;
        CancelButton.IsEnabled = !idle && !running!.IsCancellationRequested;
    }

    private async void CheckGame(object sender, RoutedEventArgs e) => await RunOperation(false);

    private async void ImportSaves(object sender, RoutedEventArgs e)
    {
        if (saveToImport is not null)
        {
            SetPendingSave(null);
            return;
        }
        var picker = new OpenFolderDialog { Title = "Escolhe a pasta de saves que contém slot_00, slot_01, etc." };
        if (picker.ShowDialog(this) != true) return;
        var destination = DestinationBox.Text;
        try
        {
            var slots = SaveImporter.GetSlots(picker.FolderName);
            if (!Builder.CanUpdate(destination))
            {
                SetPendingSave(picker.FolderName, slots.Length);
                return;
            }
            running = new CancellationTokenSource();
            RefreshButtons();
            Status.Text = await CopySavesAsync(picker.FolderName, destination);
            Detail.Text = "Os saves estão disponíveis ao abrir o jogo.";
        }
        catch (Exception error)
        {
            Status.Text = "Não foi possível importar os saves";
            Detail.Text = error.Message;
        }
        finally { FinishUpdateOperation(); }
    }

    private void SetPendingSave(string? path, int slots = 0)
    {
        saveToImport = path;
        SaveSelection.Visibility = path is null ? Visibility.Collapsed : Visibility.Visible;
        SaveSelection.Text = path is null ? "" : $"{slots} slots selecionados. Serão importados depois da compilação.";
        ImportButton.Content = path is null ? "Importar saves" : "Limpar seleção";
    }

    private async Task<string> CopySavesAsync(string source, string destination)
    {
        var result = await Task.Run(() => builder.ImportSaves(source, destination));
        if (result.Backup is not null) AppendLog($"Cópia de segurança dos saves em {result.Backup}");
        return $"Partidas importadas: {result.Slots}";
    }

    private async void CheckUpdates(object sender, RoutedEventArgs e)
    {
        running = new CancellationTokenSource();
        RefreshButtons();
        ShowBuild(this, new RoutedEventArgs());
        Status.Text = "A procurar atualizações";
        Detail.Text = "";
        ProgressBar.Value = 0;
        Percent.Text = "";
        ProgressBar.IsIndeterminate = true;
        string? package = null;
        var destination = DestinationBox.Text;
        try
        {
            var release = await ReleaseUpdater.FindAsync(running.Token);
            if (release is null)
            {
                Status.Text = "O launcher está atualizado";
                if (Builder.NeedsGameUpdate(destination))
                    Detail.Text = "Seleciona Atualizar jogo para aplicar esta versão à instalação.";
                return;
            }
            var gameUpdate = Builder.CanUpdate(destination) &&
                (!Version.TryParse(Builder.InstalledVersion(destination), out var installed) || installed < release.Version);
            var message = gameUpdate
                ? "Descarregar a nova versão? Se houver alterações ao motor, o Jinra atualiza o jogo na pasta atual e reutiliza os ficheiros existentes."
                : "Descarregar e abrir a nova versão do launcher?";
            if (MessageBox.Show(this, message, $"Jinra-Recomp {release.Version}", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            {
                Status.Text = $"Versão {release.Version} disponível";
                return;
            }
            Status.Text = $"A descarregar Jinra-Recomp {release.Version}";
            ProgressBar.IsIndeterminate = false;
            var progress = new Progress<double>(fraction => { ProgressBar.Value = fraction; Percent.Text = $"{fraction:P0}"; });
            package = await ReleaseUpdater.DownloadAsync(release, builder.Home, progress, running.Token);
            Directory.CreateDirectory(builder.Home);
            await File.WriteAllLinesAsync(Preferences, [destination, SourceBox.Text], running.Token);
            using var process = ReleaseUpdater.Start(package, builder.Home, gameUpdate ? "--apply-update" : "--open-update",
                gameUpdate ? destination : null);
        }
        catch (OperationCanceledException) { Status.Text = "Atualização cancelada"; package = null; }
        catch (Exception error) { Status.Text = "Não foi possível atualizar"; Detail.Text = error.Message; package = null; }
        finally { ProgressBar.IsIndeterminate = false; FinishUpdateOperation(); }
        if (package is not null && !closeAfterOperation) Close();
    }

    private void FinishUpdateOperation()
    {
        running?.Dispose();
        running = null;
        RefreshButtons();
        if (closeAfterOperation) Close();
    }

    internal async Task ApplyUpdateAsync(string destination)
    {
        DestinationBox.Text = destination;
        WarmShaders.IsChecked = false;
        EnableDlss.IsChecked = Builder.HasDlss(destination);
        EnableFsr4.IsChecked = Builder.HasFsr4(destination);
        if (Builder.NeedsGameUpdate(destination)) await RunOperation(true);
    }
    private async void BuildGame(object sender, RoutedEventArgs e)
    {
        if (EnableDlss.IsChecked == true && MessageBox.Show(this,
                "O Jinra vai descarregar e usar o SDK oficial da NVIDIA neste PC. Aceitas a licença NVIDIA RTX SDKs disponível na ligação junto à opção DLSS?",
                "NVIDIA DLSS 4.5", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;
        await RunOperation(true);
    }

    private void OpenNvidiaLicense(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(NvidiaLicense) { UseShellExecute = true });

    private void OpenAmdLicense(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(AmdLicense) { UseShellExecute = true });

    private async Task RunOperation(bool build)
    {
        if (running is not null) return;
        running = new CancellationTokenSource();
        RefreshButtons();
        ProgressBar.Value = 0;
        ProgressBar.IsIndeterminate = true;
        Percent.Text = "";
        Status.Text = build ? "A preparar a compilação" : "A verificar a cópia do jogo";
        Detail.Text = "";
        LogBox.Clear();
        try
        {
            Directory.CreateDirectory(builder.Home);
            await File.WriteAllLinesAsync(Preferences, [DestinationBox.Text, SourceBox.Text]);
            log = new StreamWriter(builder.BuildLog, append: false) { AutoFlush = true };
            var report = new Progress<BuildUpdate>(Update);
            if (build)
            {
                var profile = BuildProfile.Detect();
                HardwareInfo.Text = profile.Description;
                AppendLog(profile.LogLine);
                await builder.BuildAsync(SourceBox.Text, DestinationBox.Text, WarmShaders.IsChecked == true,
                    EnableDlss.IsChecked == true, EnableFsr4.IsChecked == true, profile, report, running.Token);
            }
            else await builder.CheckAsync(SourceBox.Text, report, running.Token);
            ProgressBar.IsIndeterminate = false;
            ProgressBar.Value = 1;
            Percent.Text = "100%";
            Status.Text = build ? "Jogo pronto a abrir" : "Cópia compatível";
            Detail.Text = build ? "Podes abrir o jogo em Jogar." : "Ocarina of Time · versão 1.1.1 (e0f3ccfa)";
            if (build && saveToImport is string selectedSave)
            {
                try
                {
                    Detail.Text = await CopySavesAsync(selectedSave, DestinationBox.Text);
                    SetPendingSave(null);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    Detail.Text = $"O jogo está pronto. A importação dos saves falhou. {error.Message}";
                    SetPendingSave(null);
                }
            }
            AppendLog(Detail.Text);
        }
        catch (OperationCanceledException)
        {
            Status.Text = "Operação cancelada";
            Detail.Text = "Podes voltar a tentar com a mesma pasta de destino.";
            AppendLog(Status.Text);
        }
        catch (Exception error)
        {
            Status.Text = build ? "A compilação parou" : "A verificação falhou";
            Detail.Text = error.Message;
            AppendLog(error.Message);
        }
        finally
        {
            ProgressBar.IsIndeterminate = false;
            FlushLog();
            log?.Dispose();
            log = null;
            running.Dispose();
            running = null;
            RefreshButtons();
            if (closeAfterOperation) Close();
        }
    }

    private void Update(BuildUpdate update)
    {
        if (running is null) return;
        if (update.Progress is double fraction)
        {
            ProgressBar.IsIndeterminate = update.IsStage;
            ProgressBar.Value = fraction;
            Percent.Text = update.IsStage ? "" : $"{fraction:P0}";
        }
        if (update.Text.Length == 0) return;
        if (update.IsStage) { Status.Text = update.Text; Detail.Text = ""; }
        else Detail.Text = update.Text.StartsWith("@@check ok ") ? update.Text[11..] : update.Text;
        if (update.Progress is null || update.IsStage) AppendLog(update.Text);
    }

    private void AppendLog(string text)
    {
        pendingLog.Enqueue(text);
        try { log?.WriteLine(text); }
        catch (IOException) { }
    }

    private void FlushLog()
    {
        if (pendingLog.Count == 0) return;
        LogBox.AppendText(string.Join(Environment.NewLine, pendingLog) + Environment.NewLine);
        pendingLog.Clear();
        if (LogBox.Text.Length > 150000) LogBox.Text = LogBox.Text[^120000..];
        LogBox.ScrollToEnd();
    }

    private void CancelOperation(object sender, RoutedEventArgs e)
    {
        CancelButton.IsEnabled = false;
        running?.Cancel();
    }

    private async void PlayGame(object sender, RoutedEventArgs e)
    {
        try
        {
            var session = builder.StartGame(DestinationBox.Text, Diagnostics.IsChecked == true, LowMemory.IsChecked == true);
            using var launched = session.Process;
            game = launched;
            RefreshButtons();
            Status.Text = "Jogo aberto";
            var exited = launched.WaitForExitAsync();
            var memory = session.MemoryLog is string file ? Builder.RecordMemoryAsync(launched, file, exited) : Task.CompletedTask;
            await exited;
            Status.Text = launched.ExitCode == 0 ? "Jogo fechado" : "O jogo terminou com um erro";
            if (launched.ExitCode != 0) Detail.Text = "Consulta logs\\jinra_log.txt na pasta do jogo.";
            try { await memory; }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or InvalidOperationException)
            {
                AppendLog($"O registo de memória parou. {error.Message}");
            }
        }
        catch (Exception error) { Status.Text = "Não foi possível abrir o jogo"; Detail.Text = error.Message; }
        finally { game = null; RefreshButtons(); }
    }

    private void ShowBuild(object sender, RoutedEventArgs e) => ChangePage(true);
    private void ShowLog(object sender, RoutedEventArgs e) => ChangePage(false);
    private void ChangePage(bool build)
    {
        BuildPage.Visibility = build ? Visibility.Visible : Visibility.Collapsed;
        LogPage.Visibility = build ? Visibility.Collapsed : Visibility.Visible;
        var selected = new SolidColorBrush(Color.FromRgb(18, 75, 125));
        BuildNav.Background = build ? selected : Brushes.Transparent;
        LogNav.Background = build ? Brushes.Transparent : selected;
    }

    private void OpenGameFolder(object sender, RoutedEventArgs e) => OpenExisting(DestinationBox.Text);
    private void OpenGameLogs(object sender, RoutedEventArgs e) => OpenExisting(Path.Combine(DestinationBox.Text, "logs"));
    private void OpenLogFile(object sender, RoutedEventArgs e) => OpenExisting(builder.BuildLog);
    private void OpenExisting(string path)
    {
        try
        {
            if (File.Exists(path) || Directory.Exists(path)) Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            else MessageBox.Show(this, "Este item fica disponível depois de começares a compilação.", Title, MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Win32Exception error) { MessageBox.Show(this, error.Message, Title); }
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (running is not null)
        {
            e.Cancel = true;
            if (MessageBox.Show(this, "Cancelar a operação e fechar a janela?", Title,
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            {
                closeAfterOperation = true;
                running.Cancel();
            }
        }
        else logTimer.Stop();
        base.OnClosing(e);
    }
}
