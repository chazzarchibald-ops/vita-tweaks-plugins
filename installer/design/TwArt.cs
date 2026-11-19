// Draws the Tweaks/Plugins Installer's bubble (sce_sys/icon0.png, 128 x 128) and
// its LiveArea (the 840 x 500 background and the 280 x 158 start gate), in the
// "Rows" design: your plugins as rows of one list, the middle one chosen.
//
// It does what the Engram Installer's make_icons.py, make_mockups.py and
// make_livearea.py do between them, in C# so that it runs on a Windows PC with
// nothing installed (design/make_art.ps1 compiles and runs it). The Vita wants
// 8-bit palette PNGs, so each image is fitted to 255 colours and dithered.
// The fonts (Righteous and Poppins, SIL Open Font License) only draw the images.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;

public static class TwArt {
	static readonly PrivateFontCollection fonts = new PrivateFontCollection();
	static FontFamily title, body;

	static readonly Color Top = Color.FromArgb(14, 16, 24), Bottom = Color.FromArgb(28, 34, 62);
	static readonly Color Text = Color.FromArgb(236, 236, 240), Dim = Color.FromArgb(168, 174, 190);
	static readonly Color Accent = Color.FromArgb(124, 192, 255);
	// edge and core of a ball: violet, teal, magenta, blue
	static readonly Color[][] Hues = {
		new[] { Color.FromArgb(92, 24, 228), Color.FromArgb(200, 130, 255) },
		new[] { Color.FromArgb(0, 124, 160), Color.FromArgb(110, 246, 200) },
		new[] { Color.FromArgb(200, 20, 140), Color.FromArgb(255, 138, 196) },
		new[] { Color.FromArgb(20, 60, 235), Color.FromArgb(70, 190, 255) },
	};

	public static void Fonts(string righteous, string poppins) {
		fonts.AddFontFile(righteous);
		fonts.AddFontFile(poppins);
		foreach (FontFamily f in fonts.Families) {
			if (f.Name.StartsWith("Righteous"))
				title = f;
			else
				body = f;
		}
	}

	// ---- drawing ----

	static Graphics Begin(Bitmap b, float scale) {
		Graphics g = Graphics.FromImage(b);
		g.SmoothingMode = SmoothingMode.AntiAlias;
		g.PixelOffsetMode = PixelOffsetMode.HighQuality;
		g.TextRenderingHint = TextRenderingHint.AntiAlias;
		g.ScaleTransform(scale, scale);
		return g;
	}

	static void Ground(Graphics g, float w, float h, Color top, Color bottom) {
		using (LinearGradientBrush b = new LinearGradientBrush(new PointF(0, -1), new PointF(0, h + 1), top, bottom))
			g.FillRectangle(b, 0, 0, w, h);
	}

	static void Glow(Graphics g, float cx, float cy, float r, Color color, float strength) {
		using (GraphicsPath p = new GraphicsPath()) {
			p.AddEllipse(cx - r, cy - r, r * 2, r * 2);
			using (PathGradientBrush b = new PathGradientBrush(p)) {
				b.CenterColor = Color.FromArgb((int)(255 * strength), color);
				b.SurroundColors = new[] { Color.FromArgb(0, color) };
				g.FillPath(b, p);
			}
		}
	}

	// A ball lit from the upper left, as Engram's are. alpha: 0 to 1.
	static void Orb(Graphics g, float cx, float cy, float r, Color[] hue, float alpha) {
		int a = (int)(255 * alpha);
		using (GraphicsPath p = new GraphicsPath()) {
			p.AddEllipse(cx - r, cy - r, r * 2, r * 2);
			using (PathGradientBrush b = new PathGradientBrush(p)) {
				b.CenterPoint = new PointF(cx - r * 0.3f, cy - r * 0.35f);
				b.CenterColor = Color.FromArgb(a, hue[1]);
				b.SurroundColors = new[] { Color.FromArgb(a, hue[0]) };
				g.FillPath(b, p);
			}
			GraphicsState state = g.Save();
			g.SetClip(p, CombineMode.Intersect);
			Glow(g, cx - r * 0.32f, cy - r * 0.4f, r * 0.55f, Color.White, 0.6f * alpha);
			g.Restore(state);
		}
	}

	static GraphicsPath Pill(float x, float y, float w, float h, float r) {
		GraphicsPath p = new GraphicsPath();
		float d = r * 2;
		p.AddArc(x, y, d, d, 180, 90);
		p.AddArc(x + w - d, y, d, d, 270, 90);
		p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
		p.AddArc(x, y + h - d, d, d, 90, 90);
		p.CloseFigure();
		return p;
	}

	// One row of the list: a glass pill, a ball, and a line of "text". 84 x 19 at scale 1.
	static void Row(Graphics g, float x, float y, float scale, Color[] hue, bool chosen, float alpha) {
		float w = 84 * scale, h = 19 * scale;
		using (GraphicsPath p = Pill(x, y, w, h, h / 2)) {
			Color fill = chosen ? Color.FromArgb((int)(77 * alpha), 110, 246, 200) : Color.FromArgb((int)(33 * alpha), 255, 255, 255);
			Color edge = chosen ? Color.FromArgb((int)(140 * alpha), 170, 255, 226) : Color.FromArgb((int)(51 * alpha), 255, 255, 255);
			using (SolidBrush b = new SolidBrush(fill))
				g.FillPath(b, p);
			using (Pen pen = new Pen(edge, 0.8f * scale))
				g.DrawPath(pen, p);
		}
		Orb(g, x + 11 * scale, y + 9.5f * scale, 6.2f * scale, hue, alpha);
		using (GraphicsPath line = Pill(x + 23 * scale, y + 7 * scale, (chosen ? 48 : 38) * scale, 5 * scale, 2.5f * scale))
		using (SolidBrush b = new SolidBrush(Color.FromArgb((int)((chosen ? 245 : 148) * alpha), 236, 236, 240)))
			g.FillPath(b, line);
	}

	// The three rows of the bubble, whose top left is at (x, y): 84 x 67 at scale 1.
	static void Rows(Graphics g, float x, float y, float scale, float alpha) {
		int[] order = { 0, 1, 2 };
		for (int i = 0; i < 3; i++)
			Row(g, x, y + i * 24 * scale, scale, Hues[order[i]], i == 1, alpha);
	}

	static float Width(Graphics g, string s, Font f) {
		return g.MeasureString(s, f, 4000, StringFormat.GenericTypographic).Width;
	}

	static void Write(Graphics g, string s, Font f, Color c, float x, float y) {
		using (SolidBrush b = new SolidBrush(c))
			g.DrawString(s, f, b, x, y, StringFormat.GenericTypographic);
	}

	// Letters with space between them, as "INSTALLER" is set. Returns the width.
	static float Spaced(Graphics g, string s, Font f, Color c, float x, float y, float gap, bool draw) {
		float at = x;
		foreach (char ch in s) {
			string one = ch.ToString();
			if (draw)
				Write(g, one, f, c, at, y);
			at += Width(g, one, f) + gap;
		}
		return at - gap - x;
	}

	static Bitmap Shrink(Bitmap big, int w, int h) {
		Bitmap small = new Bitmap(w, h, PixelFormat.Format24bppRgb);
		using (Graphics g = Graphics.FromImage(small)) {
			g.InterpolationMode = InterpolationMode.HighQualityBicubic;
			g.PixelOffsetMode = PixelOffsetMode.HighQuality;
			using (ImageAttributes wrap = new ImageAttributes()) {
				wrap.SetWrapMode(WrapMode.TileFlipXY); // no dark fringe at the edges
				g.DrawImage(big, new Rectangle(0, 0, w, h), 0, 0, big.Width, big.Height, GraphicsUnit.Pixel, wrap);
			}
		}
		return small;
	}

	// ---- the three images ----

	// The home screen cuts the icon to a circle, so what matters stays within about 56 of the middle.
	public static Bitmap Icon() {
		using (Bitmap big = new Bitmap(512, 512, PixelFormat.Format32bppArgb))
		using (Graphics g = Begin(big, 4)) {
			Ground(g, 128, 128, Color.FromArgb(12, 14, 24), Color.FromArgb(30, 38, 74));
			Glow(g, 64, 62, 64, Color.FromArgb(10, 200, 180), 0.32f);
			Rows(g, 22, 31, 1, 1);
			return Shrink(big, 128, 128);
		}
	}

	// The Vita puts its three buttons (x 273-571, y 0-65), the gate (x 275-568,
	// y 133-305) and the page curl (x 760-840, y 0-80) over the background, which
	// also shows bare for a moment as the page opens, so it is whole without them.
	public static Bitmap Background() {
		const int W = 840, H = 500;
		using (Bitmap big = new Bitmap(W * 2, H * 2, PixelFormat.Format32bppArgb))
		using (Graphics g = Begin(big, 2)) {
			Ground(g, W, H, Top, Bottom);
			Glow(g, 420, 200, 430, Color.FromArgb(10, 200, 180), 0.20f);
			Glow(g, 110, 70, 300, Color.FromArgb(120, 60, 240), 0.14f);
			Glow(g, 760, 360, 300, Color.FromArgb(220, 40, 150), 0.10f);
			// lists of rows, side by side, fading towards the edges
			const float scale = 1.6f, stepX = 150, stepY = 40;
			for (int col = -1; col < 7; col++)
				for (int row = -1; row < 12; row++) {
					float x = 420 - 84 * scale / 2 + (col - 2.5f) * stepX + stepX / 2, y = 26 + row * stepY;
					float dx = (x + 67 - 420) / 470f, dy = (y + 15 - 205) / 300f;
					float alpha = 1 - (float)Math.Pow(dx * dx + dy * dy, 0.8);
					if (alpha <= 0.04f)
						continue;
					bool chosen = (col == 2 || col == 3) && row == 4;
					Row(g, x, y, scale, Hues[((col + 8) * 3 + row + 8) % 4], chosen, Math.Min(1, alpha) * 0.85f);
				}
			// the name stands on a darker foot
			using (LinearGradientBrush foot = new LinearGradientBrush(new PointF(0, 318), new PointF(0, H + 1),
			                                                          Color.FromArgb(0, 12, 14, 24), Color.FromArgb(236, 12, 14, 24)))
				g.FillRectangle(foot, 0, 320, W, H - 320);
			using (Font name = new Font(title, 46, FontStyle.Regular, GraphicsUnit.Pixel))
			using (Font small = new Font(body, 12.5f, FontStyle.Regular, GraphicsUnit.Pixel))
			using (Font line = new Font(body, 15, FontStyle.Regular, GraphicsUnit.Pixel)) {
				Write(g, "Tweaks/Plugins", name, Color.FromArgb(150, 0, 0, 0), 37, 394);
				Write(g, "Tweaks/Plugins", name, Text, 36, 392);
				float after = 36 + Width(g, "Tweaks/Plugins", name) + 14;
				Spaced(g, "INSTALLER", small, Accent, after, 421, 4.2f, true);
				string a = "Your plugins' settings", b = "in one place";
				Write(g, a, line, Text, W - 34 - Width(g, a, line), 398);
				Write(g, b, line, Text, W - 34 - Width(g, b, line), 422);
			}
			return Shrink(big, W, H);
		}
	}

	// The gate. The Vita puts its Start button over x 73-207, y 118-152.
	public static Bitmap Gate() {
		const int W = 280, H = 158;
		using (Bitmap big = new Bitmap(W * 3, H * 3, PixelFormat.Format32bppArgb))
		using (Graphics g = Begin(big, 3)) {
			Ground(g, W, H, Color.FromArgb(12, 14, 24), Color.FromArgb(30, 38, 74));
			Glow(g, 140, 60, 150, Color.FromArgb(10, 200, 180), 0.30f);
			Rows(g, 140 - 84 * 0.42f / 2, 12, 0.42f, 0.95f);
			using (Font name = new Font(title, 31, FontStyle.Regular, GraphicsUnit.Pixel))
			using (Font small = new Font(body, 10.5f, FontStyle.Regular, GraphicsUnit.Pixel)) {
				float w = Width(g, "Tweaks/Plugins", name);
				Write(g, "Tweaks/Plugins", name, Color.FromArgb(150, 0, 0, 0), 140 - w / 2 + 1, 48);
				Write(g, "Tweaks/Plugins", name, Text, 140 - w / 2, 46);
				float sw = Spaced(g, "INSTALLER", small, Accent, 0, 0, 3.6f, false);
				Spaced(g, "INSTALLER", small, Accent, 140 - sw / 2, 88, 3.6f, true);
			}
			return Shrink(big, W, H);
		}
	}

	// ---- 255 colours, dithered ----

	class Box {
		public List<int> bins = new List<int>();
		public int lo0, hi0, lo1, hi1, lo2, hi2;
		public long count;
	}

	static void Measure(Box b, int[] hist) {
		b.lo0 = b.lo1 = b.lo2 = 255;
		b.hi0 = b.hi1 = b.hi2 = 0;
		b.count = 0;
		foreach (int bin in b.bins) {
			int r = bin >> 10, gr = (bin >> 5) & 31, bl = bin & 31;
			b.lo0 = Math.Min(b.lo0, r); b.hi0 = Math.Max(b.hi0, r);
			b.lo1 = Math.Min(b.lo1, gr); b.hi1 = Math.Max(b.hi1, gr);
			b.lo2 = Math.Min(b.lo2, bl); b.hi2 = Math.Max(b.hi2, bl);
			b.count += hist[bin];
		}
	}

	// Saves an image as an 8-bit palette PNG: a median cut of its colours (counted
	// 5 bits a channel), then Floyd-Steinberg dithering so shading doesn't band.
	public static void Save(Bitmap src, string path) {
		int w = src.Width, h = src.Height;
		byte[] px = new byte[w * h * 3];
		BitmapData bits = src.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
		for (int y = 0; y < h; y++)
			Marshal.Copy(new IntPtr(bits.Scan0.ToInt64() + (long)y * bits.Stride), px, y * w * 3, w * 3);
		src.UnlockBits(bits); // (rows are blue, green, red)

		int[] hist = new int[32768];
		long[] sumR = new long[32768], sumG = new long[32768], sumB = new long[32768];
		for (int i = 0; i < w * h; i++) {
			int b = px[i * 3], g = px[i * 3 + 1], r = px[i * 3 + 2], bin = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
			hist[bin]++;
			sumR[bin] += r; sumG[bin] += g; sumB[bin] += b;
		}
		Box first = new Box();
		for (int bin = 0; bin < 32768; bin++)
			if (hist[bin] > 0)
				first.bins.Add(bin);
		Measure(first, hist);
		List<Box> boxes = new List<Box> { first };
		while (boxes.Count < 255) {
			Box pick = null;
			long best = -1;
			foreach (Box b in boxes) { // the box with the most pixels that can still be cut
				int span = Math.Max(b.hi0 - b.lo0, Math.Max(b.hi1 - b.lo1, b.hi2 - b.lo2));
				long score = b.bins.Count > 1 ? b.count * (span + 1) : -1;
				if (score > best) { best = score; pick = b; }
			}
			if (pick == null || best < 0)
				break;
			int s0 = pick.hi0 - pick.lo0, s1 = pick.hi1 - pick.lo1, s2 = pick.hi2 - pick.lo2;
			int shift = s0 >= s1 && s0 >= s2 ? 10 : s1 >= s2 ? 5 : 0;
			pick.bins.Sort(delegate(int a, int b) { return ((a >> shift) & 31).CompareTo((b >> shift) & 31); });
			long half = pick.count / 2, run = 0;
			int cut = 1;
			for (int i = 0; i < pick.bins.Count - 1; i++) {
				run += hist[pick.bins[i]];
				cut = i + 1;
				if (run >= half)
					break;
			}
			Box other = new Box();
			other.bins = pick.bins.GetRange(cut, pick.bins.Count - cut);
			pick.bins = pick.bins.GetRange(0, cut);
			Measure(pick, hist);
			Measure(other, hist);
			boxes.Add(other);
		}
		int n = boxes.Count;
		int[] pr = new int[n], pg = new int[n], pb = new int[n];
		for (int i = 0; i < n; i++) {
			long r = 0, g = 0, b = 0, c = 0;
			foreach (int bin in boxes[i].bins) { r += sumR[bin]; g += sumG[bin]; b += sumB[bin]; c += hist[bin]; }
			pr[i] = (int)(r / c); pg[i] = (int)(g / c); pb[i] = (int)(b / c);
		}

		// dither, a row at a time, the error carried in fixed point
		byte[] index = new byte[w * h];
		int[] cache = new int[32768];
		for (int i = 0; i < 32768; i++)
			cache[i] = -1;
		float[] err = new float[(w + 2) * 3], next = new float[(w + 2) * 3];
		for (int y = 0; y < h; y++) {
			Array.Clear(next, 0, next.Length);
			for (int x = 0; x < w; x++) {
				int i = (y * w + x) * 3, e = (x + 1) * 3;
				float fr = px[i + 2] + err[e], fg = px[i + 1] + err[e + 1], fb = px[i] + err[e + 2];
				int r = Math.Max(0, Math.Min(255, (int)Math.Round(fr))), g = Math.Max(0, Math.Min(255, (int)Math.Round(fg)));
				int b = Math.Max(0, Math.Min(255, (int)Math.Round(fb)));
				int bin = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
				int pick = cache[bin];
				if (pick < 0) {
					int bestD = int.MaxValue;
					for (int k = 0; k < n; k++) {
						int dr = r - pr[k], dg = g - pg[k], db = b - pb[k], d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
						if (d < bestD) { bestD = d; pick = k; }
					}
					cache[bin] = pick;
				}
				index[y * w + x] = (byte)pick;
				float er = fr - pr[pick], eg = fg - pg[pick], eb = fb - pb[pick];
				err[e + 3] += er * 7 / 16; err[e + 4] += eg * 7 / 16; err[e + 5] += eb * 7 / 16;
				next[e - 3] += er * 3 / 16; next[e - 2] += eg * 3 / 16; next[e - 1] += eb * 3 / 16;
				next[e] += er * 5 / 16; next[e + 1] += eg * 5 / 16; next[e + 2] += eb * 5 / 16;
				next[e + 3] += er / 16; next[e + 4] += eg / 16; next[e + 5] += eb / 16;
			}
			float[] swap = err; err = next; next = swap;
		}

		using (Bitmap dst = new Bitmap(w, h, PixelFormat.Format8bppIndexed)) {
			ColorPalette pal = dst.Palette;
			for (int i = 0; i < 256; i++)
				pal.Entries[i] = i < n ? Color.FromArgb(pr[i], pg[i], pb[i]) : Color.Black;
			dst.Palette = pal;
			BitmapData to = dst.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format8bppIndexed);
			for (int y = 0; y < h; y++)
				Marshal.Copy(index, y * w, new IntPtr(to.Scan0.ToInt64() + (long)y * to.Stride), w);
			dst.UnlockBits(to);
			dst.Save(path, ImageFormat.Png);
		}
	}
}
