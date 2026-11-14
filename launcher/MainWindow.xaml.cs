using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media;
using Microsoft.Win32;

namespace JinraRecomp;

public partial class MainWindow : Window
{
    private ResourceDictionary? languageResources;
    private int selectedSaveCount;
    private const string NvidiaLicense = "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt";
    private const string AmdLicense = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/docs/license.md";
    private readonly Builder builder = new();
    private readonly Queue<string> pendingLog = new();
    private readonly System.Windows.Threading.DispatcherTimer logTimer;
    private CancellationTokenSource? running;
    private Process? game;
    private StreamWriter? log;
    private bool closeAfterOperation;
    private readonly bool installedMode;
    private string? saveToImport;
    private string Preferences => Path.Combine(builder.Home, "preferencias.txt");

    public MainWindow(string? source, string? installedDestination = null, bool repairDesktop = true)
    {
        installedMode = installedDestination is not null;
        SetupLanguage.Load(builder.Home);
        InitializeComponent();
        LoadLanguageResources();
        LanguagePicker.SelectedIndex = SetupLanguage.English ? 0 : 1;
        VersionLabel.Text = T($"Versão {Builder.Version}", $"Version {Builder.Version}");
        HardwareInfo.Text = BuildProfile.Detect().Description;
        logTimer = new(TimeSpan.FromMilliseconds(240), System.Windows.Threading.DispatcherPriority.Background,
            (_, _) => FlushLog(), Dispatcher);
        logTimer.Start();
        AllowDrop = !installedMode;
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
            var optionsFile = Path.Combine(builder.Home, "play-options.txt");
            if (File.Exists(optionsFile))
            {
                var options = File.ReadAllLines(optionsFile);
                LowMemory.IsChecked = options.Contains("low-memory");
                Diagnostics.IsChecked = options.Contains("diagnostics");
            }
        }
        catch (IOException) { }
        if (installedDestination is not null)
        {
            DestinationBox.Text = Path.GetFullPath(installedDestination);
            SetupCard.Visibility = ImportCard.Visibility = CheckButton.Visibility = BuildButton.Visibility =
                BuildLogButton.Visibility = Visibility.Collapsed;
            BuildNav.SetResourceReference(ContentProperty, "PlayNavText");
            LogHeading.SetResourceReference(System.Windows.Controls.TextBlock.TextProperty, "InstalledLogsText");
            PlayButton.Style = (Style)FindResource("Primary");
            Width = 920;
            Height = 630;
            SetInstalledStatus();
        }
        else if (Builder.Ready(DestinationBox.Text))
        {
            SelectSource(source);
        }
        else SelectSource(source ?? FindSource());

        EnableFsr4.IsChecked = Builder.HasFsr4(DestinationBox.Text);
        EnableDlss.IsChecked = Builder.HasDlss(DestinationBox.Text);
        if (repairDesktop && Builder.Ready(DestinationBox.Text)) InstallDesktopLauncher();
        ShowBuild(this, new RoutedEventArgs());
        RefreshButtons();
    }

    private static string T(string portuguese, string english) => SetupLanguage.Text(portuguese, english);

    private void LoadLanguageResources()
    {
        var dictionaries = SetupRoot.Resources.MergedDictionaries;
        if (languageResources is not null) dictionaries.Remove(languageResources);
        languageResources = new ResourceDictionary { Source = new Uri(SetupLanguage.English ? "SetupEnglish.xaml" : "SetupPortuguese.xaml", UriKind.Relative) };
        dictionaries.Add(languageResources);
    }

    private void ChangeLanguage(object sender, RoutedEventArgs e)
    {
        if (LanguagePicker is null || LanguagePicker.SelectedIndex < 0 || SetupLanguage.English == (LanguagePicker.SelectedIndex == 0)) return;
        try { SetupLanguage.Set(builder.Home, LanguagePicker.SelectedIndex == 0); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { AppendLog(error.Message); }
        LoadLanguageResources();
        VersionLabel.Text = T($"Versão {Builder.Version}", $"Version {Builder.Version}");
        HardwareInfo.Text = BuildProfile.Detect().Description;
        Status.Text = T("Escolhe a tua cópia do jogo", "Choose your game copy");
        Detail.Text = T("As ferramentas são preparadas no primeiro uso.", "Tools are prepared on first use.");
        SourceHint.Text = T("Arquivo .nx, .nup ou pasta com exefs e romfs. Podes arrastar a cópia para aqui.",
            "Select an .nx or .nup file, or a folder with exefs and romfs. You can drag the game copy here.");
        if (installedMode) SetInstalledStatus();
        else if (SourceBox.Text.Length > 0) SelectSource(SourceBox.Text);
        SetPendingSave(saveToImport, selectedSaveCount);
        RefreshButtons();
    }

    private void SetInstalledStatus()
    {
        var ready = Builder.Ready(DestinationBox.Text);
        Status.Text = ready ? T("Jogo pronto a abrir", "Game ready to play") : T("Instalação indisponível", "Installation unavailable");
        Detail.Text = ready ? "" : T("Confirma a pasta do jogo ou abre o setup para recuperar a instalação.",
            "Check the game folder or open setup to restore the installation.");
    }

    private void InstallDesktopLauncher()
    {
        try { DesktopLauncher.Install(DestinationBox.Text); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Detail.Text = T($"Não foi possível criar o launcher no Ambiente de Trabalho. {error.Message}",
                $"Could not create the desktop launcher. {error.Message}");
            AppendLog(Detail.Text);
        }
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
        Status.Text = T("Cópia selecionada", "Game copy selected");
        SourceHint.Text = T("Verifica a versão ou inicia a compilação.", "Verify the version or start the build.");
    }

    private void ChooseFile(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFileDialog { Title = T("Escolher a cópia do Zelda", "Choose your Zelda game copy"), Filter = T("Cópia Switch 2|*.nx;*.nup|Todos os ficheiros|*.*", "Switch 2 game copy|*.nx;*.nup|All files|*.*") };
        if (picker.ShowDialog(this) == true) SelectSource(picker.FileName);
    }

    private void ChooseFolder(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFolderDialog { Title = T("Escolher pasta com exefs e romfs", "Choose a folder containing exefs and romfs") };
        if (picker.ShowDialog(this) == true) SelectSource(picker.FolderName);
    }

    private void ChooseDestination(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFolderDialog { Title = T("Escolher o destino da compilação", "Choose the build destination") };
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
        LanguagePicker.IsEnabled = idle && !playing;
        FileButton.IsEnabled = FolderButton.IsEnabled = DestinationButton.IsEnabled = WarmShaders.IsEnabled = EnableDlss.IsEnabled = EnableFsr4.IsEnabled = idle;
        CheckButton.IsEnabled = idle && SourceBox.Text.Length > 0;
        var updating = Builder.CanUpdate(DestinationBox.Text);
        BuildButton.Content = updating ? T("Atualizar jogo", "Update game") : T("Compilar jogo", "Build game");
        BuildButton.IsEnabled = idle && !playing && (updating || SourceBox.Text.Length > 0);
        PlayButton.IsEnabled = idle && !playing && DestinationBox.Text.Length > 0 && Builder.Ready(DestinationBox.Text);
        Diagnostics.IsEnabled = LowMemory.IsEnabled = idle && !playing;
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
        var picker = new OpenFolderDialog { Title = T("Escolhe a pasta de saves que contém slot_00, slot_01, etc.", "Choose the save folder containing slot_00, slot_01, and the other slots.") };
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
            Detail.Text = T("Os saves estão disponíveis ao abrir o jogo.", "The saves are ready for the next time you open the game.");
        }
        catch (Exception error)
        {
            Status.Text = T("Não foi possível importar os saves", "Could not import saves");
            Detail.Text = error.Message;
        }
        finally { FinishUpdateOperation(); }
    }

    private void SetPendingSave(string? path, int slots = 0)
    {
        saveToImport = path;
        selectedSaveCount = slots;
        SaveSelection.Visibility = path is null ? Visibility.Collapsed : Visibility.Visible;
        SaveSelection.Text = path is null ? "" : T($"{slots} slots selecionados. Serão importados depois da compilação.", $"{slots} slots selected. The saves will be imported after building.");
        ImportButton.Content = path is null ? T("Importar saves", "Import saves") : T("Limpar seleção", "Clear selection");
    }

    private async Task<string> CopySavesAsync(string source, string destination)
    {
        var result = await Task.Run(() => builder.ImportSaves(source, destination));
        if (result.Backup is not null) AppendLog(T($"Cópia de segurança dos saves em {result.Backup}", $"Save backup in {result.Backup}"));
        return T($"Partidas importadas: {result.Slots}", $"Saves imported: {result.Slots}");
    }

    private async void CheckUpdates(object sender, RoutedEventArgs e)
    {
        running = new CancellationTokenSource();
        RefreshButtons();
        ShowBuild(this, new RoutedEventArgs());
        Status.Text = T("A procurar atualizações", "Checking for updates");
        Detail.Text = "";
        ProgressBar.Value = 0;
        Percent.Text = "";
        ProgressBar.IsIndeterminate = true;
        string? package = null;
        var applyPending = false;
        var destination = DestinationBox.Text;
        try
        {
            if (Builder.Ready(destination) && Builder.GameRunning(destination))
                throw new IOException(T("Fecha o jogo antes de procurar atualizações.", "Close the game before checking for updates."));
            var release = await ReleaseUpdater.FindAsync(running.Token);
            if (release is null)
            {
                Status.Text = T("O launcher está atualizado", "The launcher is up to date");
                applyPending = Builder.NeedsGameUpdate(destination);
                if (!applyPending) Detail.Text = T("Tens a versão mais recente.", "You are on the latest version.");
            }
            else
            {
                var message = T("Descarregar o novo launcher? Se o motor tiver mudado, o jogo instalado também será atualizado.",
                    "Download the new launcher? If the engine has changed, the installed game will be updated too.");
                if (MessageBox.Show(this, message, $"Jinra-Recomp {release.Version}", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
                {
                    Status.Text = T($"Versão {release.Version} disponível", $"Version {release.Version} is available");
                    return;
                }
                Status.Text = T($"A descarregar Jinra-Recomp {release.Version}", $"Downloading Jinra-Recomp {release.Version}");
                ProgressBar.IsIndeterminate = false;
                var progress = new Progress<double>(fraction => { ProgressBar.Value = fraction; Percent.Text = $"{fraction:P0}"; });
                package = await ReleaseUpdater.DownloadAsync(release, builder.Home, progress, running.Token);
                Directory.CreateDirectory(builder.Home);
                await File.WriteAllLinesAsync(Preferences, [destination, SourceBox.Text], running.Token);
                using var process = installedMode || Builder.CanUpdate(destination)
                    ? ReleaseUpdater.Start(package, builder.Home, "--apply-update", destination)
                    : ReleaseUpdater.Start(package, builder.Home, "--open-update");
            }
        }
        catch (OperationCanceledException) { Status.Text = T("Atualização cancelada", "Update canceled"); package = null; }
        catch (Exception error) { Status.Text = T("Não foi possível atualizar", "Could not update"); Detail.Text = error.Message; package = null; }
        finally { ProgressBar.IsIndeterminate = false; FinishUpdateOperation(); }
        if (package is not null && !closeAfterOperation) Close();
        else if (applyPending && !closeAfterOperation) await ApplyUpdateAsync(destination);
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
                T("O Jinra vai descarregar e usar o SDK oficial da NVIDIA neste PC. Aceitas a licença NVIDIA RTX SDKs disponível na ligação junto à opção DLSS?", "Jinra will download and use the official NVIDIA SDK on this PC. Do you accept the NVIDIA RTX SDKs license linked beside the DLSS option?"),
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
        Status.Text = installedMode ? T("A atualizar o jogo", "Updating the game") :
            build ? T("A preparar a compilação", "Preparing the build") : T("A verificar a cópia do jogo", "Verifying the game copy");
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
            Status.Text = build ? T("Jogo pronto a abrir", "Game ready to play") : T("Cópia compatível", "Supported game copy");
            Detail.Text = build ? T("Podes abrir o jogo em Jogar.", "Select Play to open the game.") : T("Ocarina of Time · versão 1.1.1 (e0f3ccfa)", "Ocarina of Time · version 1.1.1 (e0f3ccfa)");
            if (build && saveToImport is string selectedSave)
            {
                try
                {
                    Detail.Text = await CopySavesAsync(selectedSave, DestinationBox.Text);
                    SetPendingSave(null);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    Detail.Text = T($"O jogo está pronto. A importação dos saves falhou. {error.Message}", $"The game is ready. Save import failed. {error.Message}");
                    SetPendingSave(null);
                }
            }
            AppendLog(Detail.Text);
            if (build) InstallDesktopLauncher();
        }
        catch (OperationCanceledException)
        {
            Status.Text = T("Operação cancelada", "Operation canceled");
            Detail.Text = T("Podes voltar a tentar com a mesma pasta de destino.", "You can retry with the same destination folder.");
            AppendLog(Status.Text);
        }
        catch (Exception error)
        {
            Status.Text = installedMode ? T("A atualização do jogo parou", "Game update stopped") :
                build ? T("A compilação parou", "Build stopped") : T("A verificação falhou", "Verification failed");
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
            var diagnostics = Diagnostics.IsChecked == true;
            var lowMemory = LowMemory.IsChecked == true;
            var session = builder.StartGame(DestinationBox.Text, diagnostics, lowMemory);
            try
            {
                string[] options = [lowMemory ? "low-memory" : "", diagnostics ? "diagnostics" : ""];
                File.WriteAllLines(Path.Combine(builder.Home, "play-options.txt"), options);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { AppendLog(error.Message); }
            using var launched = session.Process;
            game = launched;
            RefreshButtons();
            Status.Text = T("Jogo aberto", "Game running");
            var exited = launched.WaitForExitAsync();
            var memory = session.MemoryLog is string file ? Builder.RecordMemoryAsync(launched, file, exited) : Task.CompletedTask;
            await exited;
            Status.Text = launched.ExitCode == 0 ? T("Jogo fechado", "Game closed") : T("O jogo terminou com um erro", "The game exited with an error");
            if (launched.ExitCode == 0) Detail.Text = "";
            if (launched.ExitCode != 0) Detail.Text = T("Consulta logs\\jinra_log.txt na pasta do jogo.", "See logs\\jinra_log.txt in the game folder.");
            try { await memory; }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception or InvalidOperationException)
            {
                AppendLog(T($"O registo de memória parou. {error.Message}", $"Memory logging stopped. {error.Message}"));
            }
        }
        catch (Exception error) { Status.Text = T("Não foi possível abrir o jogo", "Could not open the game"); Detail.Text = error.Message; }
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
    private void OpenMeasurements(object sender, RoutedEventArgs e) => OpenExisting(Path.Combine(DestinationBox.Text, "diagnostico"));
    private void OpenLogFile(object sender, RoutedEventArgs e) => OpenExisting(builder.BuildLog);
    private void OpenExisting(string path)
    {
        try
        {
            if (File.Exists(path) || Directory.Exists(path)) Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            else MessageBox.Show(this, T("Este item fica disponível depois de começares a compilação.", "This item becomes available once the build starts."), Title, MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Win32Exception error) { MessageBox.Show(this, error.Message, Title); }
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (running is not null)
        {
            e.Cancel = true;
            if (MessageBox.Show(this, T("Cancelar a operação e fechar a janela?", "Cancel the operation and close the window?"), Title,
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
