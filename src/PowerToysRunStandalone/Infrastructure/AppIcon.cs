using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace PowerToysRunStandalone.Infrastructure;

/// <summary>
/// Loads the magnifying-glass app icon. The .ico is used by the PE icon and
/// tray; WPF windows load the PNG variant (WPF's ImageSourceConverter fails on
/// this PNG-compressed ICO at XAML parse time, so it must be set in code).
/// </summary>
public static class AppIcon
{
    public static ImageSource? LoadWindowIcon()
    {
        try
        {
            var uri = new Uri("pack://application:,,,/Assets/hotspot.png");
            return BitmapFrame.Create(uri);
        }
        catch
        {
            return null;
        }
    }
}
