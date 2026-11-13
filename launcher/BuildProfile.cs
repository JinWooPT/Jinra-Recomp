using System.Globalization;
using System.Runtime.InteropServices;

namespace JinraRecomp;

internal sealed record BuildProfile(int Jobs, ulong TotalRam, ulong FreeRam, ulong FreeCommit, double Reserve)
{
    private const double GiB = 1024.0 * 1024 * 1024;

    internal static BuildProfile Detect()
    {
        var memory = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
        if (!GlobalMemoryStatusEx(ref memory))
            return new(1, 0, 0, 0, 1.5);

        return new(Environment.ProcessorCount, memory.TotalPhysical, memory.AvailablePhysical, memory.AvailableCommit,
            Math.Clamp(memory.TotalPhysical / GiB / 16, 1.5, 4));
    }

    internal string Description => TotalRam == 0
        ? "Compilação automática · 1 tarefa em paralelo"
        : $"Compilação automática · até {Jobs} tarefas · {TotalRam / GiB:0.#} GB de RAM";

    internal string LogLine => TotalRam == 0 ? Description
        : $"{Description} · {FreeRam / GiB:0.#} GB livres · {FreeCommit / GiB:0.#} GB de memória virtual disponível · reserva de {Reserve:0.#} GB";

    internal void Apply(Dictionary<string, string> environment)
    {
        environment["JINRA_ADAPTIVE_BUILD"] = "1";
        environment["JINRA_BUILD_CPUS"] = Jobs.ToString(CultureInfo.InvariantCulture);
        environment["JINRA_MEM_GATE"] = "1";
        environment["JINRA_MEM_RESERVE_GB"] = Reserve.ToString(CultureInfo.InvariantCulture);
        environment["JINRA_MEM_LIMIT_GB"] = "";
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatus
    {
        internal uint Length;
        internal uint Load;
        internal ulong TotalPhysical;
        internal ulong AvailablePhysical;
        internal ulong CommitLimit;
        internal ulong AvailableCommit;
        internal ulong TotalVirtual;
        internal ulong AvailableVirtual;
        internal ulong ExtendedVirtual;
    }

    [DllImport("kernel32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatus buffer);
}
