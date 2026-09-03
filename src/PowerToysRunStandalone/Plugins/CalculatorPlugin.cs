using System.Globalization;
using System.Text.RegularExpressions;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Calculator plugin — equivalent to PowerToys Run's Microsoft.Plugin.Calculator.
/// Evaluates arithmetic expressions automatically when the query looks like math.
/// Supports + - * / % ^, parentheses, unary minus, constants and common functions.
/// </summary>
public sealed partial class CalculatorPlugin : IPlugin
{
    public string Name => "Calculator";

    public string Description => "Evaluate math expressions such as 1+2*3 or sqrt(16).";

    public string ActionKeyword => "=";

    public int Priority => 2;

    public bool IsEnabled { get; set; } = true;

    private static readonly Regex LooksLikeMath = MathRegex();

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search;
        if (search.StartsWith('='))
        {
            search = search[1..].Trim();
        }

        if (search.Length == 0 || (!LooksLikeMath.IsMatch(search) && !search.StartsWith('=')))
        {
            return Array.Empty<ResultItem>();
        }

        try
        {
            var evaluator = new ExpressionEvaluator();
            var value = evaluator.Evaluate(search);
            var resultText = value.ToString("G15", CultureInfo.InvariantCulture);
            return new[]
            {
                new ResultItem
                {
                    Title = $"{search} = {resultText}",
                    Subtitle = "Enter to copy result to clipboard",
                    Icon = "ƒ",
                    PluginName = Name,
                    CopyToClipboard = true,
                    ClipboardText = resultText,
                    Execute = () => Task.CompletedTask,
                },
            };
        }
        catch
        {
            return Array.Empty<ResultItem>();
        }
    }

    [GeneratedRegex(@"[\d\s+\-*/%^().,a-zA-Z]")]
    private static partial Regex MathRegex();

    /// <summary>Small recursive-descent expression evaluator, no external dependencies.</summary>
    public sealed class ExpressionEvaluator
    {
        private readonly Dictionary<string, double> _constants = new(StringComparer.OrdinalIgnoreCase)
        {
            ["pi"] = Math.PI,
            ["e"] = Math.E,
        };

        private readonly Dictionary<string, Func<double[], double>> _functions = new(StringComparer.OrdinalIgnoreCase)
        {
            ["sqrt"] = a => Math.Sqrt(a[0]),
            ["abs"] = a => Math.Abs(a[0]),
            ["sin"] = a => Math.Sin(a[0]),
            ["cos"] = a => Math.Cos(a[0]),
            ["tan"] = a => Math.Tan(a[0]),
            ["asin"] = a => Math.Asin(a[0]),
            ["acos"] = a => Math.Acos(a[0]),
            ["atan"] = a => Math.Atan(a[0]),
            ["ln"] = a => Math.Log(a[0]),
            ["log"] = a => Math.Log10(a[0]),
            ["exp"] = a => Math.Exp(a[0]),
            ["floor"] = a => Math.Floor(a[0]),
            ["ceil"] = a => Math.Ceiling(a[0]),
            ["round"] = a => Math.Round(a[0]),
            ["trunc"] = a => Math.Truncate(a[0]),
            ["min"] = a => a.Min(),
            ["max"] = a => a.Max(),
        };

        private List<Token> _tokens = new();
        private int _position;

        public double Evaluate(string expression)
        {
            _tokens = Tokenize(expression);
            _position = 0;
            var result = ParseExpression();
            if (_position != _tokens.Count)
            {
                throw new FormatException("Invalid expression.");
            }

            if (double.IsNaN(result) || double.IsInfinity(result))
            {
                throw new FormatException("Invalid result.");
            }

            return result;
        }

        private double ParseExpression()
        {
            var left = ParseTerm();
            while (Match("+") || Match("-"))
            {
                var op = _tokens[_position - 1].Text;
                var right = ParseTerm();
                left = op == "+" ? left + right : left - right;
            }

            return left;
        }

        private double ParseTerm()
        {
            var left = ParsePower();
            while (Match("*") || Match("/") || Match("%"))
            {
                var op = _tokens[_position - 1].Text;
                var right = ParsePower();
                left = op switch
                {
                    "*" => left * right,
                    "/" => left / right,
                    "%" => left % right,
                    _ => left,
                };
            }

            return left;
        }

        private double ParsePower()
        {
            var left = ParseFactor();
            if (Match("^"))
            {
                var right = ParsePower();
                return Math.Pow(left, right);
            }

            return left;
        }

        private double ParseFactor()
        {
            if (Match("+") || Match("-"))
            {
                var sign = _tokens[_position - 1].Text == "-" ? -1d : 1d;
                return sign * ParseFactor();
            }

            return ParsePostfix();
        }

        private double ParsePostfix()
        {
            if (_position >= _tokens.Count)
            {
                throw new FormatException("Unexpected end of expression.");
            }

            var token = _tokens[_position++];

            if (token.Type == TokenType.Number)
            {
                return token.Value;
            }

            if (token.Type == TokenType.Identifier)
            {
                if (Match("("))
                {
                    var args = new List<double>();
                    if (!Match(")"))
                    {
                        do
                        {
                            args.Add(ParseExpression());
                        }
                        while (Match(","));

                        Expect(")");
                    }

                    if (!_functions.TryGetValue(token.Text, out var fn) || args.Count == 0)
                    {
                        throw new FormatException($"Unknown function '{token.Text}'.");
                    }

                    return fn(args.ToArray());
                }

                if (_constants.TryGetValue(token.Text, out var constant))
                {
                    return constant;
                }

                throw new FormatException($"Unknown identifier '{token.Text}'.");
            }

            if (token.Type == TokenType.LeftParen)
            {
                var value = ParseExpression();
                Expect(")");
                return value;
            }

            throw new FormatException($"Unexpected token '{token.Text}'.");
        }

        private bool Match(string text)
        {
            if (_position < _tokens.Count && _tokens[_position].Text == text)
            {
                _position++;
                return true;
            }

            return false;
        }

        private void Expect(string text)
        {
            if (!Match(text))
            {
                throw new FormatException($"Expected '{text}'.");
            }
        }

        private static List<Token> Tokenize(string text)
        {
            var tokens = new List<Token>();
            var i = 0;

            while (i < text.Length)
            {
                var ch = text[i];
                if (char.IsWhiteSpace(ch))
                {
                    i++;
                    continue;
                }

                if (char.IsDigit(ch) || ch == '.')
                {
                    var start = i;
                    while (i < text.Length && (char.IsDigit(text[i]) || text[i] is '.'))
                    {
                        i++;
                    }

                    var numberText = text[start..i];
                    if (!double.TryParse(numberText, NumberStyles.Float, CultureInfo.InvariantCulture, out var number))
                    {
                        throw new FormatException($"Invalid number '{numberText}'.");
                    }

                    tokens.Add(new Token(TokenType.Number, numberText, number));
                    continue;
                }

                if (char.IsLetter(ch) || ch == '_')
                {
                    var start = i;
                    while (i < text.Length && (char.IsLetterOrDigit(text[i]) || text[i] == '_'))
                    {
                        i++;
                    }

                    tokens.Add(new Token(TokenType.Identifier, text[start..i], 0));
                    continue;
                }

                switch (ch)
                {
                    case '+':
                    case '-':
                    case '*':
                    case '/':
                    case '%':
                    case '^':
                        tokens.Add(new Token(TokenType.Operator, ch.ToString(), 0));
                        break;
                    case '(':
                        tokens.Add(new Token(TokenType.LeftParen, "(", 0));
                        break;
                    case ')':
                        tokens.Add(new Token(TokenType.RightParen, ")", 0));
                        break;
                    case ',':
                        tokens.Add(new Token(TokenType.Comma, ",", 0));
                        break;
                    default:
                        throw new FormatException($"Unexpected character '{ch}'.");
                }

                i++;
            }

            return tokens;
        }

        private enum TokenType
        {
            Number,
            Identifier,
            Operator,
            LeftParen,
            RightParen,
            Comma,
        }

        private readonly record struct Token(TokenType Type, string Text, double Value);
    }
}
