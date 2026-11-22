using System.Diagnostics;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using static JinraRecomp.SetupLanguage;

namespace JinraRecomp;

internal sealed record BuildUpdate(string Text, double? Progress = null, bool IsStage = false);

internal sealed class Builder
{
    internal const string Version = "0.1.9";
    internal const string GameVersion = "0.1.9";
    private const string EngineVersion = GameVersion + "-4aee236";
    private const string ExpectedBuild = "E0F3CCFAF50065AE4EA01EB5AEB13B1A";
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true };
    private readonly string home = Path.GetFullPath(Environment.GetEnvironmentVariable("JINRA_RECOMP_HOME") ??
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Jinra-Recomp"));

    internal string Home => home;
    internal string BuildLog => Path.Combine(home, "compilacao.log");
    internal static bool HasFsr4(string output) => File.Exists(Path.Combine(output, ".jinra-fsr"));
    internal static bool HasAnr(string output) => File.Exists(Path.Combine(output, "anr", "jinra_anr_bridge.dll"));
    internal static bool HasDlss(string output) => File.Exists(Path.Combine(output, "ngx", "nvngx_dlss.dll"));
    private const string ShaderPreparationChoice = ".jinra-prepare-shaders";

    internal static bool PrepareShadersOnUpdate(string output)
    {
        try
        {
            var choice = Path.Combine(output, ShaderPreparationChoice);
            if (File.Exists(choice))
            {
                var value = File.ReadAllText(choice).Trim();
                if (value == "1") return true;
                if (value == "0") return false;
            }
            // Older versions recorded completed preparation in the game folder. The first word is the translator stamp.
            var prepared = Path.Combine(output, "jinra_prepared.bin");
            if (!File.Exists(prepared)) return false;
            using var reader = new BinaryReader(File.OpenRead(prepared));
            return reader.BaseStream.Length >= sizeof(ulong) && reader.ReadUInt64() != 0;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    internal static string? InstalledVersion(string output)
    {
        var marker = Path.Combine(output, ".jinra-built");
        return File.Exists(marker) ? File.ReadAllText(marker).Trim().Split('-', 2)[0] : null;
    }

    internal static bool NeedsGameUpdate(string output) => CanUpdate(output) &&
        (!System.Version.TryParse(InstalledVersion(output), out var installed) || installed < System.Version.Parse(GameVersion));

    internal static bool GameRunning(string output)
    {
        var executable = Path.GetFullPath(GameExecutable(output));
        foreach (var process in Process.GetProcessesByName("Jinra-Ocarina"))
        {
            using (process)
            {
                try
                {
                    if (string.Equals(process.MainModule?.FileName, executable, StringComparison.OrdinalIgnoreCase)) return true;
                }
                catch (Win32Exception) { if (!process.HasExited) return true; }
                catch (InvalidOperationException) { }
            }
        }
        return false;
    }
    private string Kit => Path.Combine(home, "kit", EngineVersion);
    private string Payload => Path.Combine(Kit, "engine");
    private string Tools => Path.Combine(Kit, "tools");
    private string Python => Path.Combine(Tools, "python", "python.exe");
    private string Importer => Path.Combine(Payload, "jinra-nx.exe");

    private sealed record Tool(string Name, string File);

    internal async Task CheckAsync(string source, IProgress<BuildUpdate> report, CancellationToken token)
    {
        await InstallPayloadAsync(token);
        if (Directory.Exists(source))
        {
            var folders = InputFolder(source);
            await VerifyMainAsync(Path.Combine(folders, "exefs", "main"), token);
            report.Report(new("Zelda 1.1.1 · e0f3ccfa"));
        }
        else if (await IdentifyAsync(source, token) == "NXAR")
        {
            await RunAsync(Importer, ["check", source], Payload, report, token);
        }
        else
        {
            await PrepareToolsAsync(report, token, pythonOnly: true);
            var probe = Path.Combine(home, "probe-" + Guid.NewGuid().ToString("N"));
            try
            {
                await RunAsync(Python, ["-I", "nx2/nup.py", source, "--extract", probe, "--only", "exefs"],
                    Payload, report, token, EngineEnvironment());
                await VerifyMainAsync(Path.Combine(probe, "exefs", "main"), token);
                report.Report(new("Zelda 1.1.1 · e0f3ccfa"));
            }
            finally { RemoveOwnedDirectory(probe); }
        }
    }

    internal async Task BuildAsync(string source, string output, bool prepareShaders, bool dlss, bool fsr4, BuildProfile profile,
        IProgress<BuildUpdate> report, CancellationToken token, bool installAnr = false)
    {
        output = Path.GetFullPath(output);
        source = string.IsNullOrWhiteSpace(source) ? "" : Path.GetFullPath(source);
        if (Path.GetPathRoot(output) == output || Inside(output, Payload) || Inside(output, Tools) ||
            (Directory.Exists(source) && (Inside(output, source) || Inside(source, output))))
            throw new InvalidOperationException(Text("Escolhe uma pasta de destino independente dos ficheiros do jogo e das ferramentas.",
                "Choose a destination folder outside the game files and tools."));

        Directory.CreateDirectory(home);
        using var builderLock = new FileStream(Path.Combine(home, "build.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        CheckDestination(output);
        if (GameRunning(output)) throw new IOException(Text("Fecha o jogo antes de atualizar esta instalação.", "Close the game before updating this installation."));
        var update = CanUpdate(output);
        if (update && System.Version.TryParse(InstalledVersion(output), out var installed) && installed > System.Version.Parse(GameVersion))
            throw new IOException(Text("Esta instalação já tem um motor mais recente. Abre o launcher mais recente para a atualizar.",
                "This installation already has a newer engine. Open the latest launcher to update it."));
        var isFolder = Directory.Exists(source);
        var kind = update ? "update" : isFolder ? "folder" : await IdentifyAsync(source, token);
        if (!update && isFolder)
        {
            source = InputFolder(source);
            await VerifyMainAsync(Path.Combine(source, "exefs", "main"), token);
        }
        await InstallPayloadAsync(token);
        await PrepareToolsAsync(report, token);

        Directory.CreateDirectory(output);
        PrepareGameLogs(output);
        var sourceStamp = Path.Combine(output, ".jinra-source");
        if (!update)
        {
            var identity = kind == "folder" ? source : FileIdentity(source);
            if (File.Exists(sourceStamp) && await File.ReadAllTextAsync(sourceStamp, token) != identity)
                throw new InvalidOperationException(Text("Este destino está associado a outra cópia. Escolhe uma pasta vazia para a nova cópia.",
                    "This destination belongs to another game copy. Choose an empty folder for the new copy."));
            await WriteAtomicAsync(sourceStamp, identity, token);
        }
        var id = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(output.ToUpperInvariant())))[..16];
        var work = Path.Combine(home, "build", id);
        var extract = Path.Combine(output, ".jinra-import");
        var extractionPartial = extract + ".partial";
        var romfs = Path.Combine(output, "romfs");
        var args = new List<string> { "-I", "nx2/make_game.py", "--out", output, "--work", work,
            "--jobs", profile.Jobs.ToString(CultureInfo.InvariantCulture) };
        if (!prepareShaders) args.Add("--no-prepare-shaders");
        try
        {
            if (update) args.AddRange(["--update", output]);
            else if (kind == "NXAR")
            {
                report.Report(new(Text("A extrair o arquivo NX", "Extracting the NX archive"), 0, true));
                RemoveOwnedDirectory(extractionPartial);
                if (!Directory.Exists(extract))
                {
                    await RunAsync(Importer, ["extract", source, extractionPartial], Payload, report, token);
                    Directory.Move(extractionPartial, extract);
                }
                if (!Directory.Exists(romfs)) Directory.Move(Path.Combine(extract, "romfs"), romfs);
                args.AddRange(["--romfs", romfs, "--exefs", Path.Combine(extract, "exefs")]);
            }
            else
            {
                args.AddRange(["--game", source]);
            }

            report.Report(new(Text("A preparar a recompilação", "Preparing the recompilation"), 0, true));
            var env = EngineEnvironment();
            profile.Apply(env);
            env["JINRA_VERSION"] = GameVersion;
            var previousTools = Path.Combine(home, "kit", "0.1.1-4aee236", "tools");
            env["JINRA_LEGACY_CPM"] = string.Join(Path.PathSeparator,
                new[] { "0.1.1", "0.1.2", "0.1.3", "0.1.4", "0.1.5", "0.1.6", "0.1.7" }.Select(version =>
                    Path.Combine(home, "kit", version + "-4aee236", "tools", "cpm")));
            env["JINRA_LEGACY_LLVM"] = Path.Combine(previousTools, "llvm");
            if (dlss)
            {
                env["JINRA_NGX_SDK"] = Path.Combine(home, "sdk", "nvidia-310.9.16");
                args.AddRange(["--fetch-ngx-sdk", "--accept-nvidia-license"]);
            }
            if (fsr4)
            {
                env["JINRA_FFX_SDK"] = Path.Combine(home, "sdk", "amd-fsr-2.4.0");
                args.AddRange(["--fsr", "--fetch-fsr-runtime", "--accept-amd-license"]);
            }
            else args.Add("--no-fsr");
            await RunAsync(Python, args, Payload, report, token, env);
            if (!File.Exists(GameExecutable(output)) || !File.Exists(Path.Combine(output, "image.bin")) ||
                !File.Exists(Path.Combine(output, "jinra_vk.dll")))
                throw new IOException(Text("A compilação terminou sem todos os ficheiros necessários para jogar.", "The build finished without all the files needed to play."));
            if (installAnr && dlss)
            {
                report.Report(new(Text("A preparar a geração de fotogramas ANR", "Preparing ANR frame generation"), null, true));
                await RunAsync(Python, ["-I", "scripts/anr_provider.py", "--game", output], Payload, report, token, env);
            }
            await WriteAtomicAsync(Path.Combine(output, ShaderPreparationChoice), prepareShaders ? "1" : "0", token);
            await WriteAtomicAsync(Path.Combine(output, ".jinra-built"), EngineVersion, token);
            RemoveOwnedDirectory(extract);
        }
        finally { RemoveOwnedDirectory(extractionPartial); }
    }

    internal static bool CanUpdate(string output) => File.Exists(Path.Combine(output, ".jinra-source")) &&
        Directory.Exists(Path.Combine(output, "romfs")) &&
        File.Exists(Path.Combine(output, ".jinra-exefs", "main"));

    private static void PrepareGameLogs(string output)
    {
        var logs = Directory.CreateDirectory(Path.Combine(output, "logs")).FullName;
        for (var i = 0; i <= 4; i++)
        {
            var name = i == 0 ? "jinra_log.txt" : $"jinra_log.{i}.txt";
            var old = Path.Combine(output, name);
            var target = Path.Combine(logs, name);
            if (File.Exists(old) && !File.Exists(target)) File.Move(old, target);
        }
    }

    internal static bool Ready(string output) => File.Exists(Path.Combine(output, ".jinra-built")) &&
        File.Exists(GameExecutable(output)) && File.Exists(Path.Combine(output, "image.bin")) &&
        File.Exists(Path.Combine(output, "jinra_vk.dll"));

    internal (int Slots, string? Backup) ImportSaves(string source, string output)
    {
        Directory.CreateDirectory(home);
        using var builderLock = new FileStream(Path.Combine(home, "build.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (!CanUpdate(output)) throw new IOException(Text("Seleciona primeiro uma instalação do Jinra-Recomp.", "Select a Jinra-Recomp installation first."));
        if (GameRunning(output)) throw new IOException(Text("Fecha o jogo antes de importar as partidas.", "Close the game before importing saves."));
        return SaveImporter.Import(source, output);
    }

    private static string GameExecutable(string output) => Path.Combine(output, "Jinra-Ocarina.exe");

    internal (Process Process, string? MemoryLog) StartGame(string output, bool diagnostics, bool lowMemory)
    {
        if (!Ready(output)) throw new InvalidOperationException(Text("Compila o jogo antes de o abrir.", "Build the game before launching it."));
        Directory.CreateDirectory(home);
        using var builderLock = new FileStream(Path.Combine(home, "build.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (GameRunning(output)) throw new IOException(Text("O jogo já está aberto.", "The game is already running."));
        var start = new ProcessStartInfo(GameExecutable(output)) { WorkingDirectory = output, UseShellExecute = false };
        start.Environment["JINRA_NO_UPDATE_CHECK"] = "1";
        start.Environment["RX_VK_PRECOMPILE"] = "0";
        if (File.Exists(Python)) start.Environment["JINRA_PROFILE_PYTHON"] = Python;
        if (lowMemory) start.Environment["RX_VK_PCACHE_LOAD"] = "0";
        string? memoryLog = null;
        if (diagnostics)
        {
            var path = Path.Combine(output, "diagnostico");
            Directory.CreateDirectory(path);
            var session = $"{DateTime.Now:yyyyMMdd-HHmmss-fff}-{Environment.ProcessId}";
            start.Environment["RX_VK_FRAME_LOG"] = Path.Combine("diagnostico", $"fotogramas-{session}.csv");
            start.Environment["JINRA_FRAME_TRACE_FILE"] = Path.Combine("diagnostico", $"pausas-{session}.csv");
            start.Environment["RX_VK_FRAMEGEN"] = "0";
            memoryLog = Path.Combine(path, $"memoria-{session}.csv");
        }
        return (Process.Start(start) ?? throw new IOException(Text("Não foi possível abrir o jogo.", "The game could not be launched.")), memoryLog);
    }

    internal static async Task RecordMemoryAsync(Process process, string file, Task exited)
    {
        await using var output = new StreamWriter(file);
        await output.WriteLineAsync("seconds,working_set_bytes,private_bytes");
        var timer = Stopwatch.StartNew();
        while (!exited.IsCompleted)
        {
            process.Refresh();
            try
            {
                var row = FormattableString.Invariant($"{timer.Elapsed.TotalSeconds:0.000},{process.WorkingSet64},{process.PrivateMemorySize64}");
                await output.WriteLineAsync(row);
                await output.FlushAsync();
            }
            catch (Exception error) when ((error is Win32Exception or InvalidOperationException) && process.HasExited)
            {
                break;
            }
            await Task.WhenAny(exited, Task.Delay(TimeSpan.FromSeconds(5)));
        }
    }

    internal async Task PrepareToolsAsync(IProgress<BuildUpdate> report, CancellationToken token, bool pythonOnly = false)
    {
        report.Report(new(Text("A preparar as ferramentas incluídas", "Preparing the bundled tools"), 0, true));
        await InstallPayloadAsync(token);
        Directory.CreateDirectory(Tools);
        await using var stream = File.OpenRead(Path.Combine(Kit, "tools.json"));
        var packages = (await JsonSerializer.DeserializeAsync<Tool[]>(stream, Json, token))!;
        foreach (var tool in packages)
        {
            if (pythonOnly && tool.Name != "python") continue;
            if (!File.Exists(Path.Combine(Tools, tool.File)))
                throw new IOException(Text($"A ferramenta {tool.Name} está em falta. Volta a abrir o executável completo.",
                    $"The {tool.Name} tool is missing. Open the full installer again."));
            report.Report(new(Text($"{tool.Name} disponível", $"{tool.Name} is ready")));
        }
    }

    private async Task InstallPayloadAsync(CancellationToken token)
    {
        if (File.Exists(Path.Combine(Kit, ".jinra-engine")) && File.Exists(Importer) &&
            File.Exists(Path.Combine(Payload, "nx2", "make_game.py"))) return;
        await Task.Run(() =>
        {
            using var archive = ZipFile.OpenRead(Path.Combine(AppContext.BaseDirectory, "kit.zip"));
            InstallZip(archive, "", Kit, EngineVersion, ".jinra-engine", token, "engine/jinra-nx.exe");
        }, token);
    }

    private Dictionary<string, string> EngineEnvironment()
    {
        var temp = Path.Combine(home, "tmp");
        Directory.CreateDirectory(temp);
        return new()
        {
            ["JINRA_MINGW"] = Path.Combine(Tools, "llvm"),
            ["RX_CPM"] = Path.Combine(Tools, "cpm"),
            ["JINRA_NO_UPDATE_CHECK"] = "1",
            ["TEMP"] = temp,
            ["TMP"] = temp,
            ["PYTHONUTF8"] = "1",
            ["PYTHONIOENCODING"] = "utf-8"
        };
    }

    private static void InstallZip(ZipArchive zip, string prefix, string target, string stamp, string marker,
        CancellationToken token, string required)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        var temp = target + ".partial-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(temp);
        try
        {
            foreach (var entry in zip.Entries)
            {
                token.ThrowIfCancellationRequested();
                if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal) || string.IsNullOrEmpty(entry.Name)) continue;
                var path = Path.GetFullPath(Path.Combine(temp, entry.FullName[prefix.Length..]));
                if (!Inside(path, temp) || path == temp) throw new IOException(Text("O pacote contém um caminho inválido.", "The package contains an invalid path."));
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                entry.ExtractToFile(path);
            }
            if (!File.Exists(Path.Combine(temp, required))) throw new IOException(Text("O pacote está incompleto.", "The package is incomplete."));
            File.WriteAllText(Path.Combine(temp, marker), stamp);
            token.ThrowIfCancellationRequested();
            if (Directory.Exists(target) && File.Exists(Path.Combine(target, required)) &&
                File.Exists(Path.Combine(target, marker)) && File.ReadAllText(Path.Combine(target, marker)) == stamp) return;
            RemoveOwnedDirectory(target);
            Directory.Move(temp, target);
        }
        finally { RemoveOwnedDirectory(temp); }
    }

    private static async Task RunAsync(string file, IEnumerable<string> args, string cwd,
        IProgress<BuildUpdate> report, CancellationToken token, Dictionary<string, string>? env = null)
    {
        token.ThrowIfCancellationRequested();
        var start = new ProcessStartInfo(file)
        {
            WorkingDirectory = cwd, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var arg in args) start.ArgumentList.Add(arg);
        if (env is not null)
        {
            foreach (var key in start.Environment.Keys.Where(k => k.StartsWith("JINRA_", StringComparison.OrdinalIgnoreCase)).ToArray())
                start.Environment.Remove(key);
            foreach (var (key, value) in env) start.Environment[key] = value;
        }
        using var process = Process.Start(start) ?? throw new IOException(Text("Não foi possível iniciar uma ferramenta.", "A build tool could not be started."));
        using var cancel = token.Register(() => { try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { } });
        string? error = null;
        async Task ReadAsync(StreamReader reader)
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                if (line.StartsWith("@@error ")) error = line[8..];
                if (line.StartsWith("@@stage ")) report.Report(new(StageName(line.Split(' ', 3)[1]), 0, true));
                else if (line.StartsWith("@@progress ") && double.TryParse(line[11..], NumberStyles.Float, CultureInfo.InvariantCulture, out var progress))
                    report.Report(new("", Math.Clamp(progress, 0, 1)));
                else if (line.StartsWith("@@check bad ")) error = line[12..];
                else if (!line.StartsWith("@@file ")) report.Report(new(line));
            }
        }
        await Task.WhenAll(ReadAsync(process.StandardOutput), ReadAsync(process.StandardError), process.WaitForExitAsync());
        token.ThrowIfCancellationRequested();
        if (process.ExitCode != 0 || error is not null)
            throw new IOException(error ?? Text($"A ferramenta {Path.GetFileName(file)} terminou com o código {process.ExitCode}. Consulta o registo.",
                $"{Path.GetFileName(file)} exited with code {process.ExitCode}. Check the log."));
    }

    private static string StageName(string stage) => stage switch
    {
        "read" => Text("A preparar os ficheiros do jogo", "Preparing the game files"),
        "lift" => Text("A traduzir o código da Switch 2", "Translating the Switch 2 code"),
        "compile" => Text("A compilar o jogo para Windows", "Building the game for Windows"),
        "finish" => Text("A montar o jogo e preparar os shaders", "Assembling the game and preparing shaders"),
        "shaders" => Text("A preparar os shaders", "Preparing shaders"),
        _ => Text("A preparar o jogo", "Preparing the game")
    };

    private static string InputFolder(string source)
    {
        foreach (var folder in new[] { source, Path.Combine(source, "base") })
            if (File.Exists(Path.Combine(folder, "exefs", "main")) && Directory.Exists(Path.Combine(folder, "romfs"))) return folder;
        throw new IOException(Text("Escolhe uma pasta que contenha exefs e romfs da versão 1.1.1.", "Choose a folder containing exefs and romfs from version 1.1.1."));
    }

    private static async Task<string> IdentifyAsync(string source, CancellationToken token)
    {
        if (!File.Exists(source)) throw new IOException(Text("Escolhe o arquivo .nx ou .nup do jogo, ou a pasta extraída.",
            "Choose the game's .nx or .nup archive or its extracted folder."));
        var bytes = new byte[4];
        await using var input = File.OpenRead(source);
        await input.ReadExactlyAsync(bytes, token);
        if (bytes.AsSpan().SequenceEqual("NXAR"u8)) return "NXAR";
        if (bytes.AsSpan().SequenceEqual("NUP\x01"u8)) return "NUP";
        throw new IOException(Text("O ficheiro escolhido não tem o formato NX ou NUP.", "The selected file is not in NX or NUP format."));
    }

    private static async Task VerifyMainAsync(string file, CancellationToken token)
    {
        var bytes = new byte[0x50];
        await using var stream = File.OpenRead(file);
        await stream.ReadExactlyAsync(bytes, token);
        if (!bytes.AsSpan(0, 4).SequenceEqual("NSO0"u8) || Convert.ToHexString(bytes.AsSpan(0x40, 16)) != ExpectedBuild)
            throw new IOException(Text("A versão suportada é o Zelda da Switch 2 1.1.1 (e0f3ccfa).", "The supported version is Zelda for Switch 2, 1.1.1 (e0f3ccfa)."));
    }

    private static string FileIdentity(string file)
    {
        var info = new FileInfo(file);
        return $"{info.FullName}\n{info.Length}\n{info.LastWriteTimeUtc.Ticks}";
    }

    private static void CheckDestination(string output)
    {
        if (!Directory.Exists(output)) return;
        if ((File.GetAttributes(output) & FileAttributes.ReparsePoint) != 0)
            throw new IOException(Text("Escolhe uma pasta de destino local sem redirecionamento.", "Choose a local destination folder without redirection."));
        if (Directory.EnumerateFileSystemEntries(output).Any() && !File.Exists(Path.Combine(output, ".jinra-source")))
            throw new IOException(Text("Escolhe uma pasta vazia ou uma instalação criada pelo Jinra-Recomp.",
                "Choose an empty folder or an existing Jinra-Recomp installation."));
        foreach (var entry in Directory.EnumerateFileSystemEntries(output))
            if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0)
                throw new IOException(Text("A pasta de destino contém um redirecionamento. Escolhe outro destino.",
                    "The destination folder contains a redirected path. Choose another destination."));
    }

    private static bool Inside(string child, string parent)
    {
        var relative = Path.GetRelativePath(parent, child);
        return !Path.IsPathRooted(relative) && relative != ".." && !relative.StartsWith(".." + Path.DirectorySeparatorChar);
    }

    private static void RemoveOwnedDirectory(string path)
    {
        if (Directory.Exists(path))
            Directory.Delete(path, recursive: (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0);
    }

    private static async Task WriteAtomicAsync(string path, string contents, CancellationToken token)
    {
        var temporary = path + ".partial-" + Guid.NewGuid().ToString("N");
        try
        {
            await File.WriteAllTextAsync(temporary, contents, token);
            File.Move(temporary, path, true);
        }
        finally { File.Delete(temporary); }
    }
}
