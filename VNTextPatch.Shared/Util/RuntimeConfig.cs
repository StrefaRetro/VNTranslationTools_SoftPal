using System;
using System.IO;
using Newtonsoft.Json;

namespace VNTextPatch.Shared.Util
{
    /// <summary>
    /// Runtime configuration loaded from VNTranslationToolsConstants.json
    /// Call RuntimeConfig.Load() early in Main() before accessing any values.
    /// </summary>
    public static class RuntimeConfig
    {
        private static bool _loaded = false;

        public static bool DebugLogging { get; private set; }
        public static string CustomFontName { get; private set; }
        public static string CustomFontFilename { get; private set; }
        public static string MonospaceFontFilename { get; private set; }
        public static int FontHeightIncrease { get; private set; }
        public static int FontYSpacingBetweenLines { get; private set; }
        public static int FontYTopPosDecrease { get; private set; }
        public static bool ProportionalFontBold { get; private set; }
        public static int ProportionalLineWidth { get; private set; }
        public static int MaxLineWidth { get; private set; }
        public static int NumLinesWarnThreshold { get; private set; }

        public static void Load()
        {
            if (_loaded)
                return;

            const string configFileName = "VNTranslationToolsConstants.json";

            if (!File.Exists(configFileName))
            {
                Console.Error.WriteLine($"Error: Configuration file not found: {configFileName}");
                Console.Error.WriteLine("Please ensure VNTranslationToolsConstants.json is in the current directory.");
                Environment.Exit(1);
            }

            string json;
            try
            {
                json = File.ReadAllText(configFileName);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Error: Failed to read configuration file: {configFileName}");
                Console.Error.WriteLine($"Details: {ex.Message}");
                Environment.Exit(1);
                return;
            }

            ConfigData config;
            try
            {
                config = JsonConvert.DeserializeObject<ConfigData>(json);
            }
            catch (JsonException ex)
            {
                Console.Error.WriteLine($"Error: Failed to parse configuration file: {configFileName}");
                Console.Error.WriteLine($"Details: {ex.Message}");
                Environment.Exit(1);
                return;
            }

            if (config == null)
            {
                Console.Error.WriteLine($"Error: Configuration file is empty or invalid: {configFileName}");
                Environment.Exit(1);
                return;
            }

            DebugLogging = config.debugLogging;
            CustomFontName = config.customFontName ?? throw new InvalidDataException("customFontName is required");
            CustomFontFilename = config.customFontFilename ?? throw new InvalidDataException("customFontFilename is required");
            MonospaceFontFilename = config.monospaceFontFilename ?? throw new InvalidDataException("monospaceFontFilename is required");
            FontHeightIncrease = config.fontHeightIncrease;
            FontYSpacingBetweenLines = config.fontYSpacingBetweenLines;
            FontYTopPosDecrease = config.fontYTopPosDecrease;
            ProportionalFontBold = config.proportionalFontBold;
            ProportionalLineWidth = config.proportionalLineWidth;
            MaxLineWidth = config.maxLineWidth;
            NumLinesWarnThreshold = config.numLinesWarnThreshold;

            _loaded = true;
        }

        private class ConfigData
        {
            public bool debugLogging { get; set; }
            public string customFontName { get; set; }
            public string customFontFilename { get; set; }
            public string monospaceFontFilename { get; set; }
            public int fontHeightIncrease { get; set; }
            public int fontYSpacingBetweenLines { get; set; }
            public int fontYTopPosDecrease { get; set; }
            public bool proportionalFontBold { get; set; }
            public int proportionalLineWidth { get; set; }
            public int maxLineWidth { get; set; }
            public int numLinesWarnThreshold { get; set; }
        }
    }
}
