using System.Globalization;
using System.IO;

namespace JinraRecomp;

internal static class SetupLanguage
{
    private static readonly bool SystemEnglish = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName != "pt";
    internal static bool English { get; private set; } = SystemEnglish;

    internal static string Text(string portuguese, string english) => English ? english : portuguese;

    internal static void Load(string home)
    {
        var path = Path.Combine(home, "setup-language.txt");
        try
        {
            English = (File.Exists(path) ? File.ReadAllText(path).Trim() : null) switch
            {
                "en" => true,
                "pt" => false,
                _ => SystemEnglish
            };
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { English = SystemEnglish; }
    }

    internal static void Set(string home, bool english)
    {
        English = english;
        Directory.CreateDirectory(home);
        File.WriteAllText(Path.Combine(home, "setup-language.txt"), english ? "en" : "pt");
    }
}
