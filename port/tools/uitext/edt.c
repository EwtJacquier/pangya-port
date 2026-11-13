// Distance from the letters for tools/uitext/fit.py (loaded with ctypes; fit.py builds it).
// The same measure runtime/text.c draws outlines with: Euclidean distance to the nearest pixel
// with coverage >= 128, softened by the pixel's own coverage at the edge.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static void edt1(const float *f, int n, float *d, int *v, float *z) {
	int k = 0;
	v[0] = 0;
	z[0] = -1e20f, z[1] = 1e20f;
	for (int q = 1; q < n; q++) {
		float s;
		for (;;) {
			s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (float)(2 * q - 2 * v[k]);
			if (s > z[k] || k == 0) break;
			k--;
		}
		if (s <= z[k]) s = z[k];
		k++;
		v[k] = q, z[k] = s, z[k + 1] = 1e20f;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < (float)q) k++;
		d[q] = (float)(q - v[k]) * (float)(q - v[k]) + f[v[k]];
	}
}

// cov: w x h coverage bytes. out: w x h bytes, the distance in eighths of a pixel (0 inside the
// letters, 255 at 31.9 px or more).
void dist8(const uint8_t *cov, int w, int h, uint8_t *out) {
	int n = w > h ? w : h;
	float *g = malloc(sizeof *g * (size_t)w * h);
	float *f = malloc(sizeof *f * (size_t)n), *d = malloc(sizeof *d * (size_t)n), *z = malloc(sizeof *z * (size_t)(n + 1));
	int *v = malloc(sizeof *v * (size_t)n);
	for (size_t i = 0; i < (size_t)w * h; i++) g[i] = cov[i] >= 128 ? 0.f : 1e12f;
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++) f[y] = g[(size_t)y * w + x];
		edt1(f, h, d, v, z);
		for (int y = 0; y < h; y++) g[(size_t)y * w + x] = d[y];
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) f[x] = g[(size_t)y * w + x];
		edt1(f, w, d, v, z);
		for (int x = 0; x < w; x++) g[(size_t)y * w + x] = d[x];
	}
	for (size_t i = 0; i < (size_t)w * h; i++) {
		float c = (float)cov[i] / 255.f;
		float dd = c >= 0.5f ? 0.f : sqrtf(g[i]) - 0.5f + (0.5f - c);
		float q = dd * 8.f + 0.5f;
		out[i] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
	}
	free(g), free(f), free(d), free(z), free(v);
}

// Colors of stacked layers that best explain orig (least squares). layers: n coverage planes of
// w x h bytes, bottom (outermost outline) first, the fill last. trow: when not NULL, the fill is
// a vertical gradient (t per row, 0 top .. 1 bottom) with a color at each end. bg: the background
// under the text (RGBA), or NULL for a transparent one (then each layer's alpha is solved too,
// else layers are opaque). out: (n + gradient) RGBA colors, 0..255.
static int solve(double *M, double *b, int m) {  // M m x m, b m x 4 (in place); 0 if singular
	for (int c = 0; c < m; c++) {
		int piv = c;
		for (int r = c + 1; r < m; r++)
			if (fabs(M[r * m + c]) > fabs(M[piv * m + c])) piv = r;
		if (fabs(M[piv * m + c]) < 1e-12) return 0;
		if (piv != c) {
			for (int k = 0; k < m; k++) {
				double t = M[c * m + k];
				M[c * m + k] = M[piv * m + k], M[piv * m + k] = t;
			}
			for (int k = 0; k < 4; k++) {
				double t = b[c * 4 + k];
				b[c * 4 + k] = b[piv * 4 + k], b[piv * 4 + k] = t;
			}
		}
		for (int r = 0; r < m; r++) {
			if (r == c) continue;
			double f = M[r * m + c] / M[c * m + c];
			for (int k = c; k < m; k++) M[r * m + k] -= f * M[c * m + k];
			for (int k = 0; k < 4; k++) b[r * 4 + k] -= f * b[c * 4 + k];
		}
	}
	for (int c = 0; c < m; c++)
		for (int k = 0; k < 4; k++) b[c * 4 + k] /= M[c * m + c];
	return 1;
}

// known: when not NULL, the fill's colors are given (RGBA, two for a gradient): only the outlines
// are solved; their entries in out are copied from it.
// Robust: pixels the layers can't explain (the original's letters are a little thinner or wider
// than the font's) weigh less on each pass (Cauchy weights of scale `robust`; 0: plain least
// squares), so a layer gets the color most of its pixels have rather than an average.
int lsq_colors(const uint8_t *layers, int n, int w, int h, const uint8_t *orig, const uint8_t *bg, const float *trow,
               const float *known, float robust, float *out) {
	int m = n + (trow ? 1 : 0);
	if (m > 8) return 0;
	double X[32] = {0};
	int mu = m;  // unknowns: the first mu entries
	if (known) {
		mu = n - 1;
		for (int i = mu; i < m; i++) {  // premultiplied unless over a background
			double a = bg ? 1 : known[(i - mu) * 4 + 3] / 255.0;
			for (int k = 0; k < 3; k++) X[i * 4 + k] = known[(i - mu) * 4 + k] * a;
			X[i * 4 + 3] = bg ? 0 : known[(i - mu) * 4 + 3];
		}
		if (mu == 0) goto done;
	}
	for (int pass = 0; pass < (robust > 0 ? 4 : 1); pass++) {
		double M[64] = {0}, b[32] = {0}, wt[8];
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				size_t p = (size_t)y * w + x;
				double above = 1;
				for (int i = n - 1; i >= 0; i--) {
					double a = layers[(size_t)i * w * h + p] / 255.0;
					wt[i] = a * above;
					above *= 1 - a;
				}
				if (trow) {  // the fill split between its top and bottom colors
					double t = trow[y], f = wt[n - 1];
					wt[n - 1] = f * (1 - t), wt[n] = f * t;
				}
				const uint8_t *o = orig + p * 4;
				double y4[4];
				if (bg) {
					const uint8_t *g = bg + p * 4;
					for (int k = 0; k < 3; k++) y4[k] = o[k] - g[k] * above;
					y4[3] = 255 * (1 - above);
				} else {
					for (int k = 0; k < 3; k++) y4[k] = o[k] * (o[3] / 255.0);
					y4[3] = o[3];
				}
				for (int i = mu; i < m; i++)
					for (int k = 0; k < 4; k++) y4[k] -= wt[i] * X[i * 4 + k];
				double om = 1;
				if (pass > 0) {
					double r2 = 0;
					for (int k = 0; k < (bg ? 3 : 4); k++) {
						double pr = 0;
						for (int i = 0; i < mu; i++) pr += wt[i] * X[i * 4 + k];
						r2 += (pr - y4[k]) * (pr - y4[k]);
					}
					om = 1 / (1 + r2 / (4.0 * robust * robust));
				}
				for (int i = 0; i < mu; i++) {
					if (wt[i] == 0) continue;
					for (int j = 0; j < mu; j++) M[i * mu + j] += om * wt[i] * wt[j];
					for (int k = 0; k < 4; k++) b[i * 4 + k] += om * wt[i] * y4[k];
				}
			}
		double tr = 0;
		for (int i = 0; i < mu; i++) tr += M[i * mu + i];
		double ridge = 1e-4 * tr / mu + 1e-9;
		for (int i = 0; i < mu; i++) M[i * mu + i] += ridge;
		if (!solve(M, b, mu)) return pass > 0;
		for (int i = 0; i < 4 * mu; i++) X[i] = b[i];
		if (bg)
			for (int i = 0; i < mu; i++) X[i * 4 + 3] = 0;  // opaque layers: alpha isn't solved
	}
done:
	for (int i = 0; i < m; i++) {
		if (i >= mu) {
			for (int k = 0; k < 4; k++) out[i * 4 + k] = known[(i - mu) * 4 + k];
			continue;
		}
		double a = bg ? 255 : X[i * 4 + 3];
		a = a < 0 ? 0 : a > 255 ? 255 : a;
		for (int k = 0; k < 3; k++) {
			double v = bg ? X[i * 4 + k] : (a > 1 ? X[i * 4 + k] * 255 / a : 0);
			out[i * 4 + k] = (float)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
		out[i * 4 + 3] = (float)a;
	}
	return 1;
}
