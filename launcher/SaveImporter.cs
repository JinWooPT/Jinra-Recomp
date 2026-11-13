using System.IO;

namespace JinraRecomp;

internal static class SaveImporter
{
    private const string UserId = "5a3c9e1f0b7d24680000000000000001";

    internal static (int Slots, string? Backup) Import(string source, string game)
    {
        source = Path.TrimEndingDirectorySeparator(Path.GetFullPath(source));
        var slots = GetSlots(source);

        var parent = Path.Combine(game, "user", "save");
        var destination = Path.Combine(parent, UserId);
        if (source.Equals(destination, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Esta pasta já é a pasta de saves do Jinra.");
        for (var folder = new DirectoryInfo(parent); folder is not null && folder.Exists; folder = folder.Parent)
            RejectLink(folder);
        Directory.CreateDirectory(parent);
        var staging = Path.Combine(parent, ".import-" + Guid.NewGuid().ToString("N"));
        var backup = Directory.Exists(destination)
            ? Path.Combine(game, "saves-backup", $"{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}") : null;
        try
        {
            Directory.CreateDirectory(staging);
            if (backup is not null) CopyFolder(new DirectoryInfo(destination), staging);
            foreach (var slot in slots)
            {
                var target = Path.Combine(staging, slot.Name);
                if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
                CopyFolder(slot, target);
            }
            foreach (var name in new[] { "account.sav", "option.sav" })
            {
                var file = new FileInfo(Path.Combine(source, name));
                if (!file.Exists) continue;
                RejectLink(file);
                file.CopyTo(Path.Combine(staging, name), overwrite: true);
            }
            if (backup is not null)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                Directory.Move(destination, backup);
            }
            try { Directory.Move(staging, destination); }
            catch
            {
                if (backup is not null) Directory.Move(backup, destination);
                throw;
            }
            return (slots.Length, backup);
        }
        finally
        {
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
        }
    }

    internal static DirectoryInfo[] GetSlots(string source)
    {
        var root = new DirectoryInfo(source);
        RejectLink(root);
        var slots = root.EnumerateDirectories()
            .Where(dir => dir.Name.StartsWith("slot_", StringComparison.OrdinalIgnoreCase) &&
                dir.Name.Length > 5 && dir.Name[5..].All(char.IsAsciiDigit) &&
                File.Exists(Path.Combine(dir.FullName, "progress.sav"))).ToArray();
        if (slots.Length == 0)
            throw new IOException("Escolhe a pasta de saves do Ocarina of Time que contém as subpastas slot_00, slot_01, etc.");
        return slots;
    }

    private static void CopyFolder(DirectoryInfo source, string destination)
    {
        RejectLink(source);
        Directory.CreateDirectory(destination);
        foreach (var file in source.EnumerateFiles())
        {
            RejectLink(file);
            file.CopyTo(Path.Combine(destination, file.Name));
        }
        foreach (var folder in source.EnumerateDirectories())
            CopyFolder(folder, Path.Combine(destination, folder.Name));
    }

    private static void RejectLink(FileSystemInfo item)
    {
        if (item.Attributes.HasFlag(FileAttributes.ReparsePoint))
            throw new IOException("Seleciona a pasta de saves original, sem atalhos ou ligações para outras pastas.");
    }
}
