// Offline replacement for Ubisoft's old BG&E "Darkroom" web page: turns the game's
// 16-character Internet code into the reply code the website used to give.
// Algorithm as reverse-engineered by the BG&E community (r/beyondgoodandevil "InternetCode DARKROOM" post;
// see also github.com/9214/daruma). Verified against daruma's 100 real save-game codes (all decode to N9J5).
namespace BGELauncher
{
    static class Darkroom
    {
        const string Alphabet = "ABCDEFGH+JKLMN\\PQRSTUVWXYZabcdefghijk&mnopqrstuvwxyz/123456789?!";
        static readonly int[] Keys = { 37, 31, 29, 23, 19, 17, 11, 7 };

        // [word, digit, mask, shift]; mask 3 = take bits (shift, shift+1) down to the bottom
        static readonly int[][] Layout =
        {
            new[] { 0, 0, 0x7E0000, 17 }, new[] { 0, 1, 0x10000, 11 }, new[] { 0, 1, 3, 3 }, new[] { 0, 2, 0x3F00, 8 },
            new[] { 1, 1, 0x7000000, 24 }, new[] { 1, 3, 0xFC0000, 18 }, new[] { 1, 4, 0x3F000, 12 },
            new[] { 1, 5, 0xFC0, 6 }, new[] { 1, 6, 0x3F, 0 }, new[] { 2, 8, 0x1E000000, 25 },
            new[] { 2, 9, 0x1000000, 19 }, new[] { 2, 9, 0x1F00, 8 }, new[] { 2, 10, 0x1F0000, 14 },
            new[] { 2, 10, 12, 2 }, new[] { 2, 11, 3, 4 }, new[] { 3, 7, 0xFC, 2 }, new[] { 3, 8, 3, 4 },
        };

        // Returns the reply code, or null if the input isn't a valid 16-character Internet code.
        public static string Decode(string code)
        {
            if (code == null) return null;
            code = code.Trim();
            if (code.Length != 16) return null;
            var d = new int[16];
            for (int i = 0; i < 16; i++) { d[i] = Alphabet.IndexOf(code[i]); if (d[i] < 0) return null; }

            for (int i = 6; i > 0; i--) SwapBits(d, 2 * i + i / 6, i % 6, 15, i - 1);

            int j = (d[15] >> 3) & 7, k = Keys[j], l = Keys[7 - j];
            for (int i = 30; i > 0; i--)
            {
                int m = (i * k + 45) % 90, n = (i * l + 45) % 90;
                SwapBits(d, m / 6, m % 6, n / 6, n % 6);
            }

            k = Keys[d[15] & 7];
            for (int i = 40; i > 0; i--) { int m = i * k % 90; d[m / 6] ^= 1 << (m % 6); }

            var w = new int[4];
            foreach (var t in Layout)
            {
                int x = t[0], b = t[1], mask = t[2], o = t[3];
                w[x] |= mask == 3 ? (int)((uint)(d[b] & (mask << o)) >> o) : (d[b] & (int)((uint)mask >> o)) << o;
            }
            uint v = (uint)w[2];
            return ((char)(65 + (v >> 24))).ToString() + ((v >> 16) & 255) + (char)(65 + ((v >> 8) & 255)) + (v & 255);
        }

        static void SwapBits(int[] d, int a, int x, int b, int y)
        {
            int p = (d[a] >> x) & 1, q = (d[b] >> y) & 1;
            d[a] = (d[a] & ~(1 << x)) | (q << x);
            d[b] = (d[b] & ~(1 << y)) | (p << y);
        }
    }
}
