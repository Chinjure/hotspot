using System.Security.Cryptography;
using System.Text;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// ValueGenerator plugin — equivalent to PowerToys Run's
/// Community.PowerToys.Run.Plugin.ValueGenerator.
/// Usage: "# guid", "# b64 hello", "# md5 hello", "# urlencode hello".
/// </summary>
public sealed class ValueGeneratorPlugin : IPlugin
{
    public string Name => "ValueGenerator";

    public string Description => "Generate GUIDs, hashes, Base64 and URL encodings (# ...).";

    public string ActionKeyword => "#";

    public int Priority => 15;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (!query.StartsWithKeyword("#"))
        {
            return Array.Empty<ResultItem>();
        }

        var text = query.Search[1..].Trim();
        if (text.Length == 0)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "Enter a value generator command",
                    Subtitle = "Example: # guid, # b64 hello, # md5 hello, # urlencode hello, # urldecode hello%20world",
                    Icon = "⚡",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        var (title, value) = Generate(text);
        if (value is null)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "Unknown value generator command",
                    Subtitle = "Try # guid, # b64 <text>, # md5 <text>, # sha256 <text>, # urlencode <text>, # urldecode <text>",
                    Icon = "⚡",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        return new[]
        {
            new ResultItem
            {
                Title = $"{title} = {value}",
                Subtitle = "Enter to copy",
                Icon = "⚡",
                PluginName = Name,
                CopyToClipboard = true,
                ClipboardText = value,
                Execute = () => Task.CompletedTask,
            },
        };
    }

    private static (string Title, string Value) Generate(string text)
    {
        if (text.Equals("guid", StringComparison.OrdinalIgnoreCase) ||
            text.Equals("uuid", StringComparison.OrdinalIgnoreCase))
        {
            var guid = Guid.NewGuid();
            return ("GUID", guid.ToString());
        }

        foreach (var (command, name, func) in new (string, string, Func<string, string>)[]
        {
            ("b64", "Base64 encode", s => Convert.ToBase64String(Encoding.UTF8.GetBytes(s))),
            ("b64d", "Base64 decode", s => Encoding.UTF8.GetString(Convert.FromBase64String(s))),
            ("md5", "MD5", s => Hash(s, MD5.Create())),
            ("sha1", "SHA1", s => Hash(s, SHA1.Create())),
            ("sha256", "SHA256", s => Hash(s, SHA256.Create())),
            ("urlencode", "URL encode", Uri.EscapeDataString),
            ("urldecode", "URL decode", Uri.UnescapeDataString),
        })
        {
            if (text.StartsWith(command + " ", StringComparison.OrdinalIgnoreCase))
            {
                var input = text[(command.Length + 1)..];
                return ($"{name}({input})", func(input));
            }
        }

        return (text, null!);
    }

    private static string Hash(string input, HashAlgorithm algorithm)
    {
        using (algorithm)
        {
            var bytes = algorithm.ComputeHash(Encoding.UTF8.GetBytes(input));
            return Convert.ToHexString(bytes).ToLowerInvariant();
        }
    }
}
