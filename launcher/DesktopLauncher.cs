using System.IO;
using System.Text;
using static JinraRecomp.SetupLanguage;

namespace JinraRecomp;

internal static class DesktopLauncher
{
    internal static void Install(string output, string? desktop = null)
    {
        desktop ??= Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        if (!Directory.Exists(desktop))
            throw new DirectoryNotFoundException(Text("Pasta do Ambiente de Trabalho indisponível.", "The desktop folder is unavailable."));

        var path = Path.Combine(desktop, "Jinra-Ocarina.exe");
        var launcher = Path.Combine(AppContext.BaseDirectory, "Jinra-Recomp.UI.exe");
        var payload = Encoding.Unicode.GetBytes(launcher + "\0" + Path.GetFullPath(output));
        var helper = Path.Combine(AppContext.BaseDirectory, "jinra-play.exe");
        var helperLength = new FileInfo(helper).Length;
        if (File.Exists(path))
        {
            using var existing = File.OpenRead(path);
            if (existing.Length < 12)
                throw new IOException(FileAlreadyExists());
            existing.Seek(-12, SeekOrigin.End);
            Span<byte> trailer = stackalloc byte[12];
            existing.ReadExactly(trailer);
            if (!trailer[4..].SequenceEqual("JINRALCH"u8) && !trailer[4..].SequenceEqual("JINRAPLY"u8))
                throw new IOException(FileAlreadyExists());
            if (trailer[4..].SequenceEqual("JINRALCH"u8) && existing.Length == helperLength + payload.Length + 12 &&
                BitConverter.ToInt32(trailer[..4]) == payload.Length)
            {
                existing.Seek(-12 - payload.Length, SeekOrigin.End);
                var current = new byte[payload.Length];
                existing.ReadExactly(current);
                if (current.AsSpan().SequenceEqual(payload)) return;
            }
        }

        var temporary = path + ".partial-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                using var stub = File.OpenRead(helper);
                stub.CopyTo(file);
                file.Write(payload);
                file.Write(BitConverter.GetBytes(payload.Length));
                file.Write("JINRALCH"u8);
            }
            File.Move(temporary, path, overwrite: true);
        }
        finally { File.Delete(temporary); }
    }

    private static string FileAlreadyExists() => Text(
        "Já existe um ficheiro Jinra-Ocarina.exe no Ambiente de Trabalho. Muda o nome desse ficheiro para criar o launcher.",
        "Jinra-Ocarina.exe already exists on the desktop. Rename that file to create the launcher.");
}
