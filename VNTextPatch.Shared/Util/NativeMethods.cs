using System;
using System.Runtime.InteropServices;
using System.Text;

namespace VNTextPatch.Shared.Util
{
    internal static class NativeMethods
    {
        [DllImport("user32", CallingConvention = CallingConvention.StdCall)]
        public static extern IntPtr GetDC(IntPtr hwnd);

        [DllImport("user32", CallingConvention = CallingConvention.StdCall)]
        public static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern IntPtr CreateFontW(
            int height,
            int width,
            int escapement,
            int orientation,
            int weight,
            bool italic,
            bool underline,
            bool strikeout,
            int charset,
            int outputPrecision,
            int clipPrecision,
            int quality,
            int pitchAndFamily,
            [MarshalAs(UnmanagedType.LPWStr)] string face
        );

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern IntPtr SelectObject(IntPtr hdc, IntPtr h);

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern bool GetCharABCWidthsFloatW(IntPtr hdc, int iFirst, int iLast, [MarshalAs(UnmanagedType.LPArray), Out] ABCFLOAT[] lpABC);

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern int GetKerningPairsW(IntPtr hdc, int nPairs, [MarshalAs(UnmanagedType.LPArray), Out] KERNINGPAIR[] lpKernPair);

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern int GetTextFaceW(IntPtr hdc, int c, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder lpName);

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern bool DeleteObject(IntPtr h);

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern int AddFontResourceExW(
            [MarshalAs(UnmanagedType.LPWStr)] string name,
            int fl,
            IntPtr res
        );

        [DllImport("gdi32", CallingConvention = CallingConvention.StdCall)]
        public static extern bool RemoveFontResourceExW(
            [MarshalAs(UnmanagedType.LPWStr)] string name,
            int fl,
            IntPtr res
        );

        public const int FR_PRIVATE = 0x10;

        [DllImport("kernel32", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);

        public const int FW_NORMAL = 400;
        public const int FW_BOLD = 700;

        public const int ANSI_CHARSET = 0;
        public const int DEFAULT_CHARSET = 1;
        public const int SHIFTJIS_CHARSET = 128;

        public const int OUT_DEFAULT_PRECIS = 0;
        public const int OUT_TT_ONLY_PRECIS = 7;
        public const int CLIP_DEFAULT_PRECIS = 0;

        public const int DEFAULT_QUALITY = 0;
        public const int ANTIALIASED_QUALITY = 4;
        public const int CLEARTYPE_NATURAL_QUALITY = 6;

        public const int DEFAULT_PITCH = 0;
        public const int FF_DONTCARE = 0 << 4;

        [StructLayout(LayoutKind.Sequential)]
        public struct KERNINGPAIR
        {
            public ushort wFirst;
            public ushort wSecond;
            public int iKernAmount;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct ABCFLOAT
        {
            public float abcfA;
            public float abcfB;
            public float abcfC;
        }

        public const int LCMAP_HALFWIDTH = 0x00400000;
        public const int LCMAP_FULLWIDTH = 0x00800000;

        [DllImport("kernel32", CallingConvention = CallingConvention.StdCall)]
        public static extern int LCMapStringEx(
            [MarshalAs(UnmanagedType.LPWStr)] string localeName,
            int mapFlags,
            [MarshalAs(UnmanagedType.LPWStr)] string sourceString,
            int sourceStringLength,
            [MarshalAs(UnmanagedType.LPWStr)] StringBuilder destStr,
            int destStringLength,
            IntPtr pVersionInformation,
            IntPtr reserved,
            IntPtr sortHandle
        );

        // Uniscribe APIs for OpenType text measurement
        [StructLayout(LayoutKind.Sequential)]
        public struct SCRIPT_ANALYSIS
        {
            public ushort flags;
            public ushort state;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct SCRIPT_ITEM
        {
            public int iCharPos;
            public SCRIPT_ANALYSIS a;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct SCRIPT_VISATTR
        {
            public ushort flags;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct GOFFSET
        {
            public int du;
            public int dv;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct ABC
        {
            public int abcA;
            public uint abcB;
            public int abcC;
        }

        [DllImport("usp10.dll", CallingConvention = CallingConvention.StdCall)]
        public static extern int ScriptItemize(
            [MarshalAs(UnmanagedType.LPWStr)] string pwcInChars,
            int cInChars,
            int cMaxItems,
            IntPtr psControl,
            IntPtr psState,
            [MarshalAs(UnmanagedType.LPArray), Out] SCRIPT_ITEM[] pItems,
            out int pcItems
        );

        [DllImport("usp10.dll", CallingConvention = CallingConvention.StdCall)]
        public static extern int ScriptShape(
            IntPtr hdc,
            ref IntPtr psc,
            [MarshalAs(UnmanagedType.LPWStr)] string pwcChars,
            int cChars,
            int cMaxGlyphs,
            ref SCRIPT_ANALYSIS psa,
            [MarshalAs(UnmanagedType.LPArray), Out] ushort[] pwOutGlyphs,
            [MarshalAs(UnmanagedType.LPArray), Out] ushort[] pwLogClust,
            [MarshalAs(UnmanagedType.LPArray), Out] SCRIPT_VISATTR[] psva,
            out int pcGlyphs
        );

        [DllImport("usp10.dll", CallingConvention = CallingConvention.StdCall)]
        public static extern int ScriptPlace(
            IntPtr hdc,
            ref IntPtr psc,
            [MarshalAs(UnmanagedType.LPArray)] ushort[] pwGlyphs,
            int cGlyphs,
            [MarshalAs(UnmanagedType.LPArray)] SCRIPT_VISATTR[] psva,
            ref SCRIPT_ANALYSIS psa,
            [MarshalAs(UnmanagedType.LPArray), Out] int[] piAdvance,
            [MarshalAs(UnmanagedType.LPArray), Out] GOFFSET[] pGoffset,
            out ABC pABC
        );

        [DllImport("usp10.dll", CallingConvention = CallingConvention.StdCall)]
        public static extern int ScriptFreeCache(ref IntPtr psc);
    }
}
