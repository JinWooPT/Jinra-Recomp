using System.IO;
using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using static JinraRecomp.SetupLanguage;

namespace JinraRecomp;

internal sealed record ReleaseUpdate(Version Version, Uri Download, long Size, string Sha256);

internal static class ReleaseUpdater
{
    private const string Repository = "JinWooPT/Jinra-Recomp";
    private const string PackageName = "Jinra-Recomp.exe";
    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(25) };

    static ReleaseUpdater()
    {
        Http.DefaultRequestHeaders.UserAgent.ParseAdd($"Jinra-Recomp/{Builder.Version}");
        Http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
    }

    private sealed record Release([property: JsonPropertyName("tag_name")] string Tag, bool Draft, bool Prerelease, Asset[] Assets);
    private sealed record Asset(string Name, long Size, string Digest, [property: JsonPropertyName("browser_download_url")] string Url);

    internal static bool OpenInstalledUpdate(string home, string? source)
    {
        try
        {
            var marker = Path.Combine(home, "updates", "current.txt");
            if (!File.Exists(marker) || !Version.TryParse(File.ReadAllText(marker).Trim(), out var version) ||
                version <= Version.Parse(Builder.Version)) return false;
            var package = Path.Combine(home, "updates", version.ToString(), PackageName);
            if (!File.Exists(package)) return false;
            using var process = Start(package, home, "--open-update", source);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            return false;
        }
    }

    internal static void RememberInstalledUpdate(string home)
    {
        var directory = Path.Combine(home, "updates");
        if (!File.Exists(Path.Combine(directory, Builder.Version, PackageName))) return;
        var marker = Path.Combine(directory, "current.txt");
        var temporary = marker + "." + Guid.NewGuid().ToString("N");
        try
        {
            if (File.Exists(marker) && Version.TryParse(File.ReadAllText(marker).Trim(), out var saved) &&
                saved >= Version.Parse(Builder.Version)) return;
            File.WriteAllText(temporary, Builder.Version);
            File.Move(temporary, marker, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        finally
        {
            try { File.Delete(temporary); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    internal static Process Start(string package, string home, string mode, string? argument)
    {
        var start = new ProcessStartInfo(package) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(package)! };
        start.Environment["JINRA_RECOMP_HOME"] = home;
        start.ArgumentList.Add(mode);
        if (argument is not null) start.ArgumentList.Add(argument);
        return Process.Start(start) ?? throw new IOException(Text("O novo launcher não abriu.", "The new launcher could not be opened."));
    }

    internal static async Task<ReleaseUpdate?> FindAsync(CancellationToken token)
    {
        using var response = await Http.GetAsync($"https://api.github.com/repos/{Repository}/releases/latest", token);
        if (response.StatusCode == HttpStatusCode.NotFound) return null;
        response.EnsureSuccessStatusCode();
        await using var content = await response.Content.ReadAsStreamAsync(token);
        return await ReadReleaseAsync(content, Version.Parse(Builder.Version), token);
    }

    internal static async Task<ReleaseUpdate?> ReadReleaseAsync(Stream content, Version current, CancellationToken token)
    {
        var release = await JsonSerializer.DeserializeAsync<Release>(content, JsonSerializerOptions.Web, token);
        if (release is null || release.Draft || release.Prerelease || release.Tag is not { Length: > 1 } || release.Tag[0] != 'v' ||
            !Version.TryParse(release.Tag[1..], out var version) || release.Tag != "v" + version || version <= current) return null;
        var asset = release.Assets?.SingleOrDefault(item => item.Name == PackageName);
        var url = $"https://github.com/{Repository}/releases/download/{release.Tag}/{PackageName}";
        if (asset is null || asset.Url != url || asset.Size is <= 0 or > 2_000_000_000 ||
            asset.Digest is not { Length: 71 } || !asset.Digest.StartsWith("sha256:", StringComparison.Ordinal) ||
            !asset.Digest[7..].All(Uri.IsHexDigit))
            throw new IOException(Text("A versão publicada ainda não tem um pacote válido para Windows.", "The published release has no valid Windows package yet."));
        return new ReleaseUpdate(version, new Uri(url), asset.Size, asset.Digest[7..]);
    }

    internal static async Task<string> DownloadAsync(ReleaseUpdate release, string home, IProgress<double> progress, CancellationToken token)
    {
        var directory = Directory.CreateDirectory(Path.Combine(home, "updates", release.Version.ToString())).FullName;
        var path = Path.Combine(directory, PackageName);
        var partial = path + "." + Guid.NewGuid().ToString("N") + ".partial";
        try
        {
            using var response = await Http.GetAsync(release.Download, HttpCompletionOption.ResponseHeadersRead, token);
            response.EnsureSuccessStatusCode();
            if (response.RequestMessage?.RequestUri is not { Scheme: "https" } final ||
                final.Host is not ("github.com" or "release-assets.githubusercontent.com" or "objects.githubusercontent.com"))
                throw new IOException(Text("O GitHub encaminhou o pacote para um endereço inesperado.", "GitHub redirected the package to an unexpected address."));
            if (response.Content.Headers.ContentLength is long length && length != release.Size)
                throw new IOException(Text("O tamanho do pacote difere do indicado na versão publicada.", "The package size differs from the published release information."));
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            await using var source = await response.Content.ReadAsStreamAsync(token);
            long received = 0;
            await using (var output = File.Open(partial, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                var buffer = new byte[256 * 1024];
                while (true)
                {
                    var count = await source.ReadAsync(buffer, token).AsTask().WaitAsync(TimeSpan.FromSeconds(30), token);
                    if (count == 0) break;
                    received += count;
                    if (received > release.Size) throw new IOException(Text("O pacote excede o tamanho indicado.", "The package exceeds the published size."));
                    hash.AppendData(buffer, 0, count);
                    await output.WriteAsync(buffer.AsMemory(0, count), token);
                    progress.Report((double)received / release.Size);
                }
            }
            if (received != release.Size || !Convert.ToHexString(hash.GetHashAndReset()).Equals(release.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new IOException(Text("A verificação do pacote falhou. Volta a tentar descarregá-lo.", "Package verification failed. Try downloading it again."));
            token.ThrowIfCancellationRequested();
            File.Move(partial, path, overwrite: true);
            return path;
        }
        finally { File.Delete(partial); }
    }
}
