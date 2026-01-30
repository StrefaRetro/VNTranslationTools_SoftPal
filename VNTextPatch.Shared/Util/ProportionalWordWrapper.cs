using System;
using System.Collections.Generic;
using System.Configuration;
using System.IO;
using System.Text;

namespace VNTextPatch.Shared.Util
{
    internal class ProportionalWordWrapper : WordWrapper, IDisposable
    {
        private static readonly string CustomFontFilePath;
        private static readonly string MonospaceFontFilePath;

        static ProportionalWordWrapper()
        {
            CustomFontFilePath = FindFontFile(RuntimeConfig.CustomFontFilename);
            if (CustomFontFilePath != null)
            {
                int result = NativeMethods.AddFontResourceExW(CustomFontFilePath, NativeMethods.FR_PRIVATE, IntPtr.Zero);
                if (result <= 0)
                    throw new FileNotFoundException($"Failed to load custom font: {CustomFontFilePath}");
            }

            MonospaceFontFilePath = FindFontFile(RuntimeConfig.MonospaceFontFilename);
            if (MonospaceFontFilePath != null)
            {
                int result = NativeMethods.AddFontResourceExW(MonospaceFontFilePath, NativeMethods.FR_PRIVATE, IntPtr.Zero);
                if (result <= 0)
                    throw new FileNotFoundException($"Failed to load custom font: {CustomFontFilePath}");
            }

            _fontName = RuntimeConfig.CustomFontName;
            _fontBold = false;
            _defaultLineWidth = RuntimeConfig.ProportionalLineWidth;

            Default = new ProportionalWordWrapper(
                _fontName,
                SharedConstants.GAME_DEFAULT_FONT_HEIGHT + RuntimeConfig.FontHeightIncrease,
                _fontBold,
                _defaultLineWidth
            );

            Secondary = new ProportionalWordWrapper(
                _fontName,
                SharedConstants.GAME_DEFAULT_FONT_HEIGHT + RuntimeConfig.FontHeightIncrease,
                _fontBold,
                _defaultLineWidth
            );

            string monospaceFontName = Path.GetFileNameWithoutExtension(RuntimeConfig.MonospaceFontFilename);
            Monospace = new ProportionalWordWrapper(
                monospaceFontName,
                SharedConstants.GAME_DEFAULT_FONT_HEIGHT + RuntimeConfig.FontHeightIncrease,
                false,
                _defaultLineWidth
            );

        }

        private static string FindFontFile(string filename, bool required = true)
        {
            string cwd = Directory.GetCurrentDirectory();
            string fontPath = Path.Combine(cwd, filename);
            if (File.Exists(fontPath))
                return fontPath;

            if (required)
                throw new FileNotFoundException($"{filename} not found in {cwd}");
            return null;
        }

        public static readonly ProportionalWordWrapper Default;
        public static readonly ProportionalWordWrapper Secondary;
        public static readonly ProportionalWordWrapper Monospace;

        private static readonly Dictionary<int, ProportionalWordWrapper> _sizeCache = new Dictionary<int, ProportionalWordWrapper>();
        private static readonly string _fontName;
        private static readonly bool _fontBold;
        private static readonly int _defaultLineWidth;

        public static ProportionalWordWrapper GetForSize(int fontSize)
        {
            if (_sizeCache.TryGetValue(fontSize, out var wrapper))
                return wrapper;

            wrapper = new ProportionalWordWrapper(_fontName, fontSize, _fontBold, _defaultLineWidth);
            _sizeCache[fontSize] = wrapper;
            return wrapper;
        }

        private readonly IntPtr _dc;
        private readonly IntPtr _font;
        private IntPtr _scriptCache;

        public ProportionalWordWrapper(string fontName, int fontSize, bool bold, int lineWidth)
        {
            _dc = NativeMethods.GetDC(IntPtr.Zero);
            _font = NativeMethods.CreateFontW(
                fontSize,
                0,
                0,
                0,
                bold ? NativeMethods.FW_BOLD : NativeMethods.FW_NORMAL,
                false,
                false,
                false,
                NativeMethods.ANSI_CHARSET,
                NativeMethods.OUT_DEFAULT_PRECIS,
                NativeMethods.CLIP_DEFAULT_PRECIS,
                NativeMethods.DEFAULT_QUALITY,
                NativeMethods.DEFAULT_PITCH | NativeMethods.FF_DONTCARE,
                fontName
            );
            NativeMethods.SelectObject(_dc, _font);

            StringBuilder actualFontName = new StringBuilder(256);
            NativeMethods.GetTextFaceW(_dc, actualFontName.Capacity, actualFontName);
            if (RuntimeConfig.DebugLogging)
            {
                Console.WriteLine($"Requested font: {fontName}, actual font: {actualFontName}");
            }

            LineWidth = lineWidth;

            _scriptCache = IntPtr.Zero;

            if (RuntimeConfig.DebugLogging) Console.WriteLine($"ProportionalWordWrapper: font={fontName}, size={fontSize}, bold={bold}, lineWidth={lineWidth}");
        }

        protected override int GetTextWidth(string text, int offset, int length)
        {
            if (length <= 0)
                return 0;

            // Use Uniscribe for text measurement with OpenType kerning/GPOS support
            string substring = text.Substring(offset, length);
            return GetStringAdvance(substring);
        }

        private int GetStringAdvance(string text)
        {
            if (string.IsNullOrEmpty(text))
                return 0;

            // ScriptItemize: break text into runs
            NativeMethods.SCRIPT_ITEM[] items = new NativeMethods.SCRIPT_ITEM[text.Length + 1];
            int numItems;
            int hr = NativeMethods.ScriptItemize(text, text.Length, items.Length, IntPtr.Zero, IntPtr.Zero, items, out numItems);
            if (hr < 0 || numItems <= 0)
                return 0;

            int totalWidth = 0;

            // Process each run
            for (int itemIdx = 0; itemIdx < numItems; itemIdx++)
            {
                int charPos = items[itemIdx].iCharPos;
                int runLength = (itemIdx + 1 < numItems) ? items[itemIdx + 1].iCharPos - charPos : text.Length - charPos;
                if (runLength <= 0)
                    continue;

                string runText = text.Substring(charPos, runLength);
                NativeMethods.SCRIPT_ANALYSIS sa = items[itemIdx].a;

                // ScriptShape: convert characters to glyphs
                int maxGlyphs = runLength * 3;
                ushort[] glyphs = new ushort[maxGlyphs];
                ushort[] logClust = new ushort[runLength];
                NativeMethods.SCRIPT_VISATTR[] visAttrs = new NativeMethods.SCRIPT_VISATTR[maxGlyphs];
                int numGlyphs;

                hr = NativeMethods.ScriptShape(_dc, ref _scriptCache, runText, runLength, maxGlyphs, ref sa, glyphs, logClust, visAttrs, out numGlyphs);
                if (hr < 0 || numGlyphs <= 0)
                    continue;

                // ScriptPlace: get glyph advances
                int[] advances = new int[numGlyphs];
                NativeMethods.GOFFSET[] offsets = new NativeMethods.GOFFSET[numGlyphs];
                NativeMethods.ABC abc;

                hr = NativeMethods.ScriptPlace(_dc, ref _scriptCache, glyphs, numGlyphs, visAttrs, ref sa, advances, offsets, out abc);
                if (hr < 0)
                    continue;

                for (int i = 0; i < numGlyphs; i++)
                {
                    totalWidth += advances[i];
                }
            }

            return totalWidth;
        }

        protected override int LineWidth
        {
            get;
        }

        public void Dispose()
        {
            if (_scriptCache != IntPtr.Zero)
            {
                NativeMethods.ScriptFreeCache(ref _scriptCache);
            }
            NativeMethods.ReleaseDC(IntPtr.Zero, _dc);
            NativeMethods.DeleteObject(_font);
        }
    }
}
