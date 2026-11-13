using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace JinraRecomp;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (e.Args is ["--probe", var source, var log])
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = ProbeAsync(source, log);
            return;
        }
        if (e.Args is ["--prepare-tools", var toolsLog])
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = ProbeAsync(null, toolsLog);
            return;
        }
        if (e.Args is ["--preview", var screenshot])
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            var window = new MainWindow(null);
            var content = (FrameworkElement)window.Content;
            window.Content = null;
            content.Measure(new Size(976, 676));
            content.Arrange(new Rect(0, 0, 976, 676));
            content.UpdateLayout();
            var bitmap = new RenderTargetBitmap(976, 676, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(content);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using (var file = File.Create(screenshot)) encoder.Save(file);
            window.Close();
            Shutdown();
            return;
        }
        var update = e.Args is ["--apply-update", _];
        var updatedLauncher = e.Args is ["--open-update"] or ["--open-update", _];
        var sourcePath = update ? null : updatedLauncher ? e.Args.ElementAtOrDefault(1) : e.Args.FirstOrDefault();
        var home = new Builder().Home;
        if (update || updatedLauncher) ReleaseUpdater.RememberInstalledUpdate(home);
        else if (ReleaseUpdater.OpenInstalledUpdate(home, sourcePath)) { Shutdown(); return; }
        var launcher = new MainWindow(sourcePath);
        MainWindow = launcher;
        launcher.Show();
        if (update) _ = launcher.ApplyUpdateAsync(e.Args[1]);
    }

    private async Task ProbeAsync(string? source, string log)
    {
        int result = 0;
        using var output = File.CreateText(log);
        try
        {
            var builder = new Builder();
            var report = new Progress<BuildUpdate>(x => output.WriteLine(x.Text));
            if (source is null) await builder.PrepareToolsAsync(report, CancellationToken.None);
            else await builder.CheckAsync(source, report, CancellationToken.None);
            await Task.Yield();
            output.WriteLine("OK");
        }
        catch (Exception error)
        {
            output.WriteLine(error);
            result = 1;
        }
        Shutdown(result);
    }
}
