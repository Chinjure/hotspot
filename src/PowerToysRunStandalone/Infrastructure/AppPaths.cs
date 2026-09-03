using System.IO;

namespace PowerToysRunStandalone.Infrastructure;

/// <summary>
/// Resolves the per-user data directory. The app is named "hotspot"; for
/// backward compatibility the old "%LocalAppData%\PowerToysRunStandalone"
/// folder (settings + NTFS index) is migrated once to
/// "%LocalAppData%\hotspot".
/// </summary>
public static class AppPaths
{
    private static readonly Lazy<string> DataDirectory = new(ResolveDataDirectory);

    public static string DataDir => DataDirectory.Value;

    private static string ResolveDataDirectory()
    {
        var root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var newDirectory = Path.Combine(root, "hotspot");
        var oldDirectory = Path.Combine(root, "PowerToysRunStandalone");

        if (!Directory.Exists(newDirectory) && Directory.Exists(oldDirectory))
        {
            try
            {
                Directory.Move(oldDirectory, newDirectory);
                return newDirectory;
            }
            catch
            {
                // Move failed (locked file etc.): keep using the old location.
                return oldDirectory;
            }
        }

        return newDirectory;
    }
}
