using System.Globalization;
using System.Text.RegularExpressions;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Unit Converter plugin — equivalent to PowerToys Run's
/// Microsoft.Plugin.UnitConverter. Usage: "u 100 km to mi" or "100kg to lb".
/// </summary>
public sealed partial class UnitConverterPlugin : IPlugin
{
    public string Name => "Unit Converter";

    public string Description => "Convert units, e.g. u 100 km to mi or 10 usd? (currency not bundled).";

    public string ActionKeyword => "u";

    public int Priority => 3;

    public bool IsEnabled { get; set; } = true;

    private static readonly Regex ConvertPattern = ConvertExpression();

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var text = query.Search;
        if (text.StartsWith("u ", StringComparison.OrdinalIgnoreCase))
        {
            text = text[2..].Trim();
        }

        var match = ConvertPattern.Match(text);
        if (!match.Success)
        {
            return Array.Empty<ResultItem>();
        }

        var valueText = match.Groups["value"].Value;
        var from = match.Groups["from"].Value;
        var to = match.Groups["to"].Value;
        if (!double.TryParse(valueText, NumberStyles.Float, CultureInfo.InvariantCulture, out var value))
        {
            return Array.Empty<ResultItem>();
        }

        var result = Convert(value, from, to);
        if (!result.HasValue)
        {
            return Array.Empty<ResultItem>();
        }

        var converted = result.Value;
        var fromName = UnitName(from);
        var toName = UnitName(to);
        return new[]
        {
            new ResultItem
            {
                Title = $"{valueText} {fromName} = {converted.ToString("G12", CultureInfo.InvariantCulture)} {toName}",
                Subtitle = $"{fromName} to {toName}",
                Icon = "⇄",
                PluginName = Name,
                CopyToClipboard = true,
                ClipboardText = converted.ToString("G12", CultureInfo.InvariantCulture),
                Execute = () => Task.CompletedTask,
            },
        };
    }

    private static double? Convert(double value, string from, string to)
    {
        var fromUnit = Resolve(from);
        var toUnit = Resolve(to);
        if (fromUnit is null || toUnit is null)
        {
            return null;
        }

        // Temperature needs special handling because it is not a simple linear
        // factor against a single base unit.
        if (fromUnit.Category == UnitCategory.Temperature && toUnit.Category == UnitCategory.Temperature)
        {
            return fromUnit.ToBase(value) switch
            {
                var celsius when toUnit.Symbol == "k" => celsius + 273.15,
                var celsius when toUnit.Symbol == "f" => celsius * 9 / 5 + 32,
                var celsius => celsius,
            };
        }

        if (fromUnit.Category != toUnit.Category)
        {
            return null;
        }

        var baseValue = fromUnit.Factor * value;
        return baseValue / toUnit.Factor;
    }

    private static Unit? Resolve(string symbol) => Units.FirstOrDefault(u =>
        string.Equals(u.Symbol, symbol, StringComparison.OrdinalIgnoreCase) ||
        string.Equals(u.Alias, symbol, StringComparison.OrdinalIgnoreCase));

    private static string UnitName(string symbol) => Resolve(symbol)?.Name ?? symbol;

    private static readonly Unit[] Units =
    {
        // Length
        new(UnitCategory.Length, "m", "meter", "m", 1),
        new(UnitCategory.Length, "km", "kilometer", "km", 1000),
        new(UnitCategory.Length, "cm", "centimeter", "cm", 0.01),
        new(UnitCategory.Length, "mm", "millimeter", "mm", 0.001),
        new(UnitCategory.Length, "um", "micrometer", "µm", 0.000001),
        new(UnitCategory.Length, "mi", "mile", "mile", 1609.344),
        new(UnitCategory.Length, "yd", "yard", "yd", 0.9144),
        new(UnitCategory.Length, "ft", "foot", "feet", 0.3048),
        new(UnitCategory.Length, "in", "inch", "inches", 0.0254),
        new(UnitCategory.Length, "nmi", "nautical mile", "nmi", 1852),

        // Mass
        new(UnitCategory.Mass, "kg", "kilogram", "kg", 1),
        new(UnitCategory.Mass, "g", "gram", "g", 0.001),
        new(UnitCategory.Mass, "mg", "milligram", "mg", 0.000001),
        new(UnitCategory.Mass, "t", "tonne", "t", 1000),
        new(UnitCategory.Mass, "lb", "pound", "lbs", 0.45359237),
        new(UnitCategory.Mass, "oz", "ounce", "oz", 0.028349523125),
        new(UnitCategory.Mass, "st", "stone", "st", 6.35029318),

        // Temperature
        new(UnitCategory.Temperature, "c", "celsius", "c", 1, v => v, v => v),
        new(UnitCategory.Temperature, "f", "fahrenheit", "f", 1, v => (v - 32) * 5 / 9, v => v),
        new(UnitCategory.Temperature, "k", "kelvin", "k", 1, v => v - 273.15, v => v),

        // Speed
        new(UnitCategory.Speed, "m/s", "meter per second", "mps", 1),
        new(UnitCategory.Speed, "km/h", "kilometer per hour", "kmh", 1 / 3.6),
        new(UnitCategory.Speed, "mph", "mile per hour", "mph", 0.44704),
        new(UnitCategory.Speed, "knot", "knot", "kn", 0.514444),

        // Time
        new(UnitCategory.Time, "s", "second", "sec", 1),
        new(UnitCategory.Time, "ms", "millisecond", "ms", 0.001),
        new(UnitCategory.Time, "min", "minute", "min", 60),
        new(UnitCategory.Time, "h", "hour", "hr", 3600),
        new(UnitCategory.Time, "day", "day", "d", 86400),
        new(UnitCategory.Time, "week", "week", "wk", 604800),

        // Data
        new(UnitCategory.Data, "b", "byte", "byte", 1),
        new(UnitCategory.Data, "kb", "kilobyte", "KB", 1024),
        new(UnitCategory.Data, "mb", "megabyte", "MB", 1024 * 1024),
        new(UnitCategory.Data, "gb", "gigabyte", "GB", 1024d * 1024 * 1024),
        new(UnitCategory.Data, "tb", "terabyte", "TB", 1024d * 1024 * 1024 * 1024),

        // Area
        new(UnitCategory.Area, "m2", "square meter", "m²", 1),
        new(UnitCategory.Area, "km2", "square kilometer", "km²", 1_000_000),
        new(UnitCategory.Area, "ft2", "square foot", "ft²", 0.09290304),
        new(UnitCategory.Area, "acre", "acre", "ac", 4046.8564224),

        // Volume
        new(UnitCategory.Volume, "l", "liter", "L", 0.001),
        new(UnitCategory.Volume, "ml", "milliliter", "mL", 0.000001),
        new(UnitCategory.Volume, "m3", "cubic meter", "m³", 1),
        new(UnitCategory.Volume, "gal", "gallon (US)", "gal", 0.003785411784),

        // Energy
        new(UnitCategory.Energy, "j", "joule", "J", 1),
        new(UnitCategory.Energy, "kj", "kilojoule", "kJ", 1000),
        new(UnitCategory.Energy, "cal", "calorie", "cal", 4.184),
        new(UnitCategory.Energy, "kcal", "kilocalorie", "kcal", 4184),
        new(UnitCategory.Energy, "wh", "watt hour", "Wh", 3600),
        new(UnitCategory.Energy, "kwh", "kilowatt hour", "kWh", 3_600_000),
        new(UnitCategory.Energy, "btu", "british thermal unit", "BTU", 1055.05585262),

        // Pressure
        new(UnitCategory.Pressure, "pa", "pascal", "Pa", 1),
        new(UnitCategory.Pressure, "kpa", "kilopascal", "kPa", 1000),
        new(UnitCategory.Pressure, "mpa", "megapascal", "MPa", 1_000_000),
        new(UnitCategory.Pressure, "bar", "bar", "bar", 100_000),
        new(UnitCategory.Pressure, "psi", "psi", "psi", 6894.757293168),
        new(UnitCategory.Pressure, "atm", "atmosphere", "atm", 101325),
        new(UnitCategory.Pressure, "torr", "torr", "torr", 133.322368421),

        // Power
        new(UnitCategory.Power, "w", "watt", "W", 1),
        new(UnitCategory.Power, "kw", "kilowatt", "kW", 1000),
        new(UnitCategory.Power, "hp", "horsepower", "hp", 745.699871582),
    };

    private enum UnitCategory
    {
        Length,
        Mass,
        Temperature,
        Speed,
        Time,
        Data,
        Area,
        Volume,
        Energy,
        Pressure,
        Power,
    }

    private sealed class Unit
    {
        public Unit(UnitCategory category, string symbol, string name, string alias, double factor)
        {
            Category = category;
            Symbol = symbol;
            Name = name;
            Alias = alias;
            Factor = factor;
            ToBase = v => v * factor;
            FromBase = v => v / factor;
        }

        public Unit(UnitCategory category, string symbol, string name, string alias, double factor,
            Func<double, double> toBase, Func<double, double> fromBase)
        {
            Category = category;
            Symbol = symbol;
            Name = name;
            Alias = alias;
            Factor = factor;
            ToBase = toBase;
            FromBase = fromBase;
        }

        public UnitCategory Category { get; }
        public string Symbol { get; }
        public string Name { get; }
        public string Alias { get; }
        public double Factor { get; }
        public Func<double, double> ToBase { get; }
        public Func<double, double> FromBase { get; }
    }

    [GeneratedRegex(@"^(?<value>-?\d+(?:\.\d+)?)\s*(?<from>[a-zA-Zµ/²³]+)\s*(?:to|in|->|→)\s*(?<to>[a-zA-Zµ/²³]+)\s*$", RegexOptions.IgnoreCase)]
    private static partial Regex ConvertExpression();
}
