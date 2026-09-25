/* vDSP: the part of Accelerate a program actually calls — elementwise
 * vector arithmetic, reductions and the FFT.
 *
 * These are ordinary computations, so they are implemented rather than
 * stubbed: a spectrum display or a level meter that gets zeros back is
 * worse than one that is simply slower than Apple's hand-tuned code. The
 * transforms follow vDSP's conventions exactly, including its scaling:
 * a forward real transform (`zrip`, `zrop`) produces twice the
 * mathematical result, and no inverse divides by N — the caller does.
 *
 * Split complex is vDSP's layout: separate real and imaginary arrays. */
#include "synthesise.h"

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <vector>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

namespace {

template <typename T> struct split_complex { T *realp; T *imagp; };
using DSPSplitComplex = split_complex<float>;
using DSPDoubleSplitComplex = split_complex<double>;

/* vDSP strides are in elements and may be negative, so indexing goes
 * through this rather than pointer arithmetic on the caller's behalf. */
template <typename T> T &at(T *p, std::int64_t stride, std::int64_t i) { return p[stride * i]; }

/* An FFT setup is just the size it was created for; the twiddle factors
 * are cheap enough to compute per transform, and a program creates one
 * setup and reuses it, so nothing here is on a hot path that Apple's
 * precomputed tables would change asymptotically. */
struct fft_setup { std::uint64_t max_log2n; };

/* In-place iterative radix-2 Cooley-Tukey. `sign` is -1 for the forward
 * transform (vDSP's FFT_FORWARD) and +1 for the inverse. */
template <typename T>
void fft_in_place(T *re, T *im, std::int64_t stride, std::uint64_t log2n, int sign) {
    const std::size_t n = std::size_t{1} << log2n;
    if (n < 2) return;
    /* bit-reversal permutation */
    for (std::size_t i = 1, j = 0; i < n; i++) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(at(re, stride, static_cast<std::int64_t>(i)), at(re, stride, static_cast<std::int64_t>(j)));
            std::swap(at(im, stride, static_cast<std::int64_t>(i)), at(im, stride, static_cast<std::int64_t>(j)));
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double theta = sign * 2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> step(std::cos(theta), std::sin(theta));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; k++) {
                const auto a = static_cast<std::int64_t>(i + k), b = static_cast<std::int64_t>(i + k + len / 2);
                const std::complex<double> u(at(re, stride, a), at(im, stride, a));
                const std::complex<double> v = std::complex<double>(at(re, stride, b), at(im, stride, b)) * w;
                at(re, stride, a) = static_cast<T>((u + v).real());
                at(im, stride, a) = static_cast<T>((u + v).imag());
                at(re, stride, b) = static_cast<T>((u - v).real());
                at(im, stride, b) = static_cast<T>((u - v).imag());
                w *= step;
            }
        }
    }
}

/* The real-input transform. vDSP packs the N/2+1 spectrum of an N-point
 * real signal into N/2 complex values: DC in realp[0], Nyquist in
 * imagp[0], and the rest as usual — and everything scaled by 2. The input
 * is the signal split into even samples (realp) and odd samples (imagp),
 * which is what vDSP_ctoz produces. */
template <typename T> void real_forward(T *re, T *im, std::int64_t stride, std::uint64_t log2n) {
    const std::size_t half = std::size_t{1} << (log2n - 1);
    if (half == 0) return;
    fft_in_place(re, im, stride, log2n - 1, -1);
    /* Untangle the half-size complex transform into the real spectrum. */
    std::vector<std::complex<double>> z(half);
    for (std::size_t k = 0; k < half; k++)
        z[k] = {at(re, stride, static_cast<std::int64_t>(k)), at(im, stride, static_cast<std::int64_t>(k))};
    const double theta = -std::numbers::pi / static_cast<double>(half);
    for (std::size_t k = 0; k < half; k++) {
        const std::complex<double> zk = z[k], zc = std::conj(z[(half - k) % half]);
        const std::complex<double> even = (zk + zc) * 0.5;
        const std::complex<double> odd = (zk - zc) * std::complex<double>(0, -0.5);
        const std::complex<double> tw(std::cos(theta * static_cast<double>(k)), std::sin(theta * static_cast<double>(k)));
        const std::complex<double> value = even + tw * odd;
        if (k == 0) {
            /* DC and Nyquist share the first element, both scaled by 2. */
            at(re, stride, 0) = static_cast<T>(2.0 * (even.real() + odd.real()));
            at(im, stride, 0) = static_cast<T>(2.0 * (even.real() - odd.real()));
        } else {
            at(re, stride, static_cast<std::int64_t>(k)) = static_cast<T>(2.0 * value.real());
            at(im, stride, static_cast<std::int64_t>(k)) = static_cast<T>(2.0 * value.imag());
        }
    }
}

/* The inverse of real_forward: packed spectrum back to even/odd samples.
 * vDSP leaves the result scaled by N, which the caller removes. */
template <typename T> void real_inverse(T *re, T *im, std::int64_t stride, std::uint64_t log2n) {
    const std::size_t half = std::size_t{1} << (log2n - 1);
    if (half == 0) return;
    /* The packed spectrum carries the forward transform's factor of 2, and
     * the half-size inverse below contributes N/2 rather than N; the two
     * cancel, so the values are used as they stand and the result comes out
     * scaled by N, which is what vDSP promises. */
    std::vector<std::complex<double>> spectrum(half + 1);
    spectrum[0] = {at(re, stride, 0), 0.0};
    spectrum[half] = {at(im, stride, 0), 0.0};
    for (std::size_t k = 1; k < half; k++)
        spectrum[k] = {at(re, stride, static_cast<std::int64_t>(k)), at(im, stride, static_cast<std::int64_t>(k))};
    /* Rebuild the half-size complex sequence, then transform it back. */
    std::vector<std::complex<double>> z(half);
    const double theta = std::numbers::pi / static_cast<double>(half);
    for (std::size_t k = 0; k < half; k++) {
        const std::complex<double> a = spectrum[k], b = std::conj(spectrum[half - k]);
        const std::complex<double> even = (a + b) * 0.5;
        const std::complex<double> tw(std::cos(theta * static_cast<double>(k)), std::sin(theta * static_cast<double>(k)));
        const std::complex<double> odd = (a - b) * 0.5 * tw;
        z[k] = even + std::complex<double>(0, 1) * odd;
    }
    for (std::size_t k = 0; k < half; k++) {
        at(re, stride, static_cast<std::int64_t>(k)) = static_cast<T>(z[k].real());
        at(im, stride, static_cast<std::int64_t>(k)) = static_cast<T>(z[k].imag());
    }
    fft_in_place(re, im, stride, log2n - 1, +1);
}

/* vDSP_DFT setups carry the length and the direction they were made for. */
struct dft_setup { std::uint64_t length; int direction; bool real_to_complex; };

template <typename T>
void dft_execute(const dft_setup *setup, const T *ir, const T *ii, T *or_, T *oi) {
    if (!setup) return;
    const std::uint64_t n = setup->length;
    std::uint64_t log2n = 0;
    while ((std::uint64_t{1} << log2n) < n) log2n++;
    std::vector<T> re(n / 2), im(n / 2);
    if (setup->direction == 1) { /* forward: the input is the real signal, split even/odd */
        for (std::uint64_t i = 0; i < n / 2; i++) { re[i] = ir[i]; im[i] = ii[i]; }
        real_forward(re.data(), im.data(), 1, log2n);
    } else {
        for (std::uint64_t i = 0; i < n / 2; i++) { re[i] = ir[i]; im[i] = ii[i]; }
        real_inverse(re.data(), im.data(), 1, log2n);
    }
    for (std::uint64_t i = 0; i < n / 2; i++) { or_[i] = re[i]; oi[i] = im[i]; }
}

} // namespace

extern "C" {

/* ---- setup lifecycle ------------------------------------------------------- */

void *vDSP_create_fftsetup_(std::uint64_t log2n, std::int32_t) DARWIN(vDSP_create_fftsetup);
void *vDSP_create_fftsetup_(std::uint64_t log2n, std::int32_t) { return new fft_setup{log2n}; }
void *vDSP_create_fftsetupD_(std::uint64_t log2n, std::int32_t) DARWIN(vDSP_create_fftsetupD);
void *vDSP_create_fftsetupD_(std::uint64_t log2n, std::int32_t) { return new fft_setup{log2n}; }
void vDSP_destroy_fftsetup_(void *s) DARWIN(vDSP_destroy_fftsetup);
void vDSP_destroy_fftsetup_(void *s) { delete static_cast<fft_setup *>(s); }
void vDSP_destroy_fftsetupD_(void *s) DARWIN(vDSP_destroy_fftsetupD);
void vDSP_destroy_fftsetupD_(void *s) { delete static_cast<fft_setup *>(s); }

/* ---- interleaved <-> split complex ------------------------------------------ */

void vDSP_ctoz_(const float *c, std::int64_t cstride, DSPSplitComplex *z, std::int64_t zstride, std::uint64_t n) DARWIN(vDSP_ctoz);
void vDSP_ctoz_(const float *c, std::int64_t cstride, DSPSplitComplex *z, std::int64_t zstride, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        at(z->realp, zstride, static_cast<std::int64_t>(i)) = c[cstride * static_cast<std::int64_t>(i)];
        at(z->imagp, zstride, static_cast<std::int64_t>(i)) = c[cstride * static_cast<std::int64_t>(i) + 1];
    }
}
void vDSP_ctozD_(const double *c, std::int64_t cstride, DSPDoubleSplitComplex *z, std::int64_t zstride, std::uint64_t n) DARWIN(vDSP_ctozD);
void vDSP_ctozD_(const double *c, std::int64_t cstride, DSPDoubleSplitComplex *z, std::int64_t zstride, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        at(z->realp, zstride, static_cast<std::int64_t>(i)) = c[cstride * static_cast<std::int64_t>(i)];
        at(z->imagp, zstride, static_cast<std::int64_t>(i)) = c[cstride * static_cast<std::int64_t>(i) + 1];
    }
}
void vDSP_ztoc_(const DSPSplitComplex *z, std::int64_t zstride, float *c, std::int64_t cstride, std::uint64_t n) DARWIN(vDSP_ztoc);
void vDSP_ztoc_(const DSPSplitComplex *z, std::int64_t zstride, float *c, std::int64_t cstride, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        c[cstride * static_cast<std::int64_t>(i)] = at(z->realp, zstride, static_cast<std::int64_t>(i));
        c[cstride * static_cast<std::int64_t>(i) + 1] = at(z->imagp, zstride, static_cast<std::int64_t>(i));
    }
}
void vDSP_ztocD_(const DSPDoubleSplitComplex *z, std::int64_t zstride, double *c, std::int64_t cstride, std::uint64_t n) DARWIN(vDSP_ztocD);
void vDSP_ztocD_(const DSPDoubleSplitComplex *z, std::int64_t zstride, double *c, std::int64_t cstride, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        c[cstride * static_cast<std::int64_t>(i)] = at(z->realp, zstride, static_cast<std::int64_t>(i));
        c[cstride * static_cast<std::int64_t>(i) + 1] = at(z->imagp, zstride, static_cast<std::int64_t>(i));
    }
}

/* ---- transforms -------------------------------------------------------------- */

void vDSP_fft_zip_(void *, DSPSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) DARWIN(vDSP_fft_zip);
void vDSP_fft_zip_(void *, DSPSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) {
    fft_in_place(c->realp, c->imagp, stride, log2n, direction == 1 ? -1 : +1);
}
void vDSP_fft_zipD_(void *, DSPDoubleSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) DARWIN(vDSP_fft_zipD);
void vDSP_fft_zipD_(void *, DSPDoubleSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) {
    fft_in_place(c->realp, c->imagp, stride, log2n, direction == 1 ? -1 : +1);
}
void vDSP_fft_zrip_(void *, DSPSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) DARWIN(vDSP_fft_zrip);
void vDSP_fft_zrip_(void *, DSPSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) {
    if (direction == 1) real_forward(c->realp, c->imagp, stride, log2n);
    else real_inverse(c->realp, c->imagp, stride, log2n);
}
void vDSP_fft_zripD_(void *, DSPDoubleSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) DARWIN(vDSP_fft_zripD);
void vDSP_fft_zripD_(void *, DSPDoubleSplitComplex *c, std::int64_t stride, std::uint64_t log2n, std::int32_t direction) {
    if (direction == 1) real_forward(c->realp, c->imagp, stride, log2n);
    else real_inverse(c->realp, c->imagp, stride, log2n);
}

/* The DFT interface: zrop is real-to-complex, out of place. */
void *vDSP_DFT_zrop_CreateSetup_(void *, std::uint64_t length, std::int64_t direction) DARWIN(vDSP_DFT_zrop_CreateSetup);
void *vDSP_DFT_zrop_CreateSetup_(void *, std::uint64_t length, std::int64_t direction) {
    return new dft_setup{length, static_cast<int>(direction), true};
}
void *vDSP_DFT_zrop_CreateSetupD_(void *, std::uint64_t length, std::int64_t direction) DARWIN(vDSP_DFT_zrop_CreateSetupD);
void *vDSP_DFT_zrop_CreateSetupD_(void *, std::uint64_t length, std::int64_t direction) {
    return new dft_setup{length, static_cast<int>(direction), true};
}
void vDSP_DFT_DestroySetup_(void *s) DARWIN(vDSP_DFT_DestroySetup);
void vDSP_DFT_DestroySetup_(void *s) { delete static_cast<dft_setup *>(s); }
void vDSP_DFT_DestroySetupD_(void *s) DARWIN(vDSP_DFT_DestroySetupD);
void vDSP_DFT_DestroySetupD_(void *s) { delete static_cast<dft_setup *>(s); }

void vDSP_DFT_Execute_(void *setup, const float *ir, const float *ii, float *or_, float *oi) DARWIN(vDSP_DFT_Execute);
void vDSP_DFT_Execute_(void *setup, const float *ir, const float *ii, float *or_, float *oi) {
    dft_execute(static_cast<const dft_setup *>(setup), ir, ii, or_, oi);
}
void vDSP_DFT_ExecuteD_(void *setup, const double *ir, const double *ii, double *or_, double *oi) DARWIN(vDSP_DFT_ExecuteD);
void vDSP_DFT_ExecuteD_(void *setup, const double *ir, const double *ii, double *or_, double *oi) {
    dft_execute(static_cast<const dft_setup *>(setup), ir, ii, or_, oi);
}

/* ---- elementwise vector arithmetic --------------------------------------------- */

#define MM_VDSP_BINARY(name, expr)                                                                                     \
    void name##_(const float *a, std::int64_t sa, const float *b, std::int64_t sb, float *c, std::int64_t sc,           \
                 std::uint64_t n) DARWIN(name);                                                                        \
    void name##_(const float *a, std::int64_t sa, const float *b, std::int64_t sb, float *c, std::int64_t sc,           \
                 std::uint64_t n) {                                                                                    \
        for (std::uint64_t i = 0; i < n; i++) {                                                                        \
            const float x = at(a, sa, static_cast<std::int64_t>(i)), y = at(b, sb, static_cast<std::int64_t>(i));       \
            at(c, sc, static_cast<std::int64_t>(i)) = (expr);                                                           \
        }                                                                                                              \
    }

MM_VDSP_BINARY(vDSP_vadd, x + y)
MM_VDSP_BINARY(vDSP_vsub, y - x) /* vDSP subtracts the first from the second */
MM_VDSP_BINARY(vDSP_vmul, x * y)
MM_VDSP_BINARY(vDSP_vdiv, y / x) /* likewise: C = B / A */
MM_VDSP_BINARY(vDSP_vmax, x > y ? x : y)
MM_VDSP_BINARY(vDSP_vmin, x < y ? x : y)

void vDSP_vabs_(const float *a, std::int64_t sa, float *c, std::int64_t sc, std::uint64_t n) DARWIN(vDSP_vabs);
void vDSP_vabs_(const float *a, std::int64_t sa, float *c, std::int64_t sc, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++)
        at(c, sc, static_cast<std::int64_t>(i)) = std::fabs(at(a, sa, static_cast<std::int64_t>(i)));
}
void vDSP_vsmul_(const float *a, std::int64_t sa, const float *b, float *c, std::int64_t sc, std::uint64_t n) DARWIN(vDSP_vsmul);
void vDSP_vsmul_(const float *a, std::int64_t sa, const float *b, float *c, std::int64_t sc, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++)
        at(c, sc, static_cast<std::int64_t>(i)) = at(a, sa, static_cast<std::int64_t>(i)) * *b;
}
void vDSP_vsmulD_(const double *a, std::int64_t sa, const double *b, double *c, std::int64_t sc, std::uint64_t n) DARWIN(vDSP_vsmulD);
void vDSP_vsmulD_(const double *a, std::int64_t sa, const double *b, double *c, std::int64_t sc, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++)
        at(c, sc, static_cast<std::int64_t>(i)) = at(a, sa, static_cast<std::int64_t>(i)) * *b;
}
void vDSP_vma_(const float *a, std::int64_t sa, const float *b, std::int64_t sb, const float *c, std::int64_t sc,
               float *d, std::int64_t sd, std::uint64_t n) DARWIN(vDSP_vma);
void vDSP_vma_(const float *a, std::int64_t sa, const float *b, std::int64_t sb, const float *c, std::int64_t sc,
               float *d, std::int64_t sd, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        const auto k = static_cast<std::int64_t>(i);
        at(d, sd, k) = at(a, sa, k) * at(b, sb, k) + at(c, sc, k);
    }
}

/* ---- reductions ------------------------------------------------------------------ */

void vDSP_dotpr_(const float *a, std::int64_t sa, const float *b, std::int64_t sb, float *out, std::uint64_t n) DARWIN(vDSP_dotpr);
void vDSP_dotpr_(const float *a, std::int64_t sa, const float *b, std::int64_t sb, float *out, std::uint64_t n) {
    double sum = 0;
    for (std::uint64_t i = 0; i < n; i++)
        sum += static_cast<double>(at(a, sa, static_cast<std::int64_t>(i))) * at(b, sb, static_cast<std::int64_t>(i));
    *out = static_cast<float>(sum);
}
void vDSP_meanv_(const float *a, std::int64_t sa, float *out, std::uint64_t n) DARWIN(vDSP_meanv);
void vDSP_meanv_(const float *a, std::int64_t sa, float *out, std::uint64_t n) {
    double sum = 0;
    for (std::uint64_t i = 0; i < n; i++) sum += at(a, sa, static_cast<std::int64_t>(i));
    *out = n ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
}
void vDSP_rmsqv_(const float *a, std::int64_t sa, float *out, std::uint64_t n) DARWIN(vDSP_rmsqv);
void vDSP_rmsqv_(const float *a, std::int64_t sa, float *out, std::uint64_t n) {
    double sum = 0;
    for (std::uint64_t i = 0; i < n; i++) {
        const double v = at(a, sa, static_cast<std::int64_t>(i));
        sum += v * v;
    }
    *out = n ? static_cast<float>(std::sqrt(sum / static_cast<double>(n))) : 0.0f;
}
void vDSP_maxvi_(const float *a, std::int64_t sa, float *value, std::uint64_t *index, std::uint64_t n) DARWIN(vDSP_maxvi);
void vDSP_maxvi_(const float *a, std::int64_t sa, float *value, std::uint64_t *index, std::uint64_t n) {
    float best = -HUGE_VALF;
    std::uint64_t best_at = 0;
    for (std::uint64_t i = 0; i < n; i++) {
        const float v = at(a, sa, static_cast<std::int64_t>(i));
        if (v > best) { best = v; best_at = i * static_cast<std::uint64_t>(sa); }
    }
    if (value) *value = n ? best : 0.0f;
    if (index) *index = best_at;
}
void vDSP_minvi_(const float *a, std::int64_t sa, float *value, std::uint64_t *index, std::uint64_t n) DARWIN(vDSP_minvi);
void vDSP_minvi_(const float *a, std::int64_t sa, float *value, std::uint64_t *index, std::uint64_t n) {
    float best = HUGE_VALF;
    std::uint64_t best_at = 0;
    for (std::uint64_t i = 0; i < n; i++) {
        const float v = at(a, sa, static_cast<std::int64_t>(i));
        if (v < best) { best = v; best_at = i * static_cast<std::uint64_t>(sa); }
    }
    if (value) *value = n ? best : 0.0f;
    if (index) *index = best_at;
}

/* ---- split-complex arithmetic ------------------------------------------------------ */

void vDSP_zvadd_(const DSPSplitComplex *a, std::int64_t sa, const DSPSplitComplex *b, std::int64_t sb,
                 const DSPSplitComplex *c, std::int64_t sc, std::uint64_t n) DARWIN(vDSP_zvadd);
void vDSP_zvadd_(const DSPSplitComplex *a, std::int64_t sa, const DSPSplitComplex *b, std::int64_t sb,
                 const DSPSplitComplex *c, std::int64_t sc, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        const auto k = static_cast<std::int64_t>(i);
        at(c->realp, sc, k) = at(a->realp, sa, k) + at(b->realp, sb, k);
        at(c->imagp, sc, k) = at(a->imagp, sa, k) + at(b->imagp, sb, k);
    }
}
void vDSP_zvsub_(const DSPSplitComplex *a, std::int64_t sa, const DSPSplitComplex *b, std::int64_t sb,
                 const DSPSplitComplex *c, std::int64_t sc, std::uint64_t n) DARWIN(vDSP_zvsub);
void vDSP_zvsub_(const DSPSplitComplex *a, std::int64_t sa, const DSPSplitComplex *b, std::int64_t sb,
                 const DSPSplitComplex *c, std::int64_t sc, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        const auto k = static_cast<std::int64_t>(i);
        at(c->realp, sc, k) = at(a->realp, sa, k) - at(b->realp, sb, k);
        at(c->imagp, sc, k) = at(a->imagp, sa, k) - at(b->imagp, sb, k);
    }
}
void vDSP_zvma_(const DSPSplitComplex *a, std::int64_t sa, const DSPSplitComplex *b, std::int64_t sb,
                const DSPSplitComplex *c, std::int64_t sc, const DSPSplitComplex *d, std::int64_t sd,
                std::uint64_t n) DARWIN(vDSP_zvma);
void vDSP_zvma_(const DSPSplitComplex *a, std::int64_t sa, const DSPSplitComplex *b, std::int64_t sb,
                const DSPSplitComplex *c, std::int64_t sc, const DSPSplitComplex *d, std::int64_t sd,
                std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        const auto k = static_cast<std::int64_t>(i);
        const float ar = at(a->realp, sa, k), ai = at(a->imagp, sa, k);
        const float br = at(b->realp, sb, k), bi = at(b->imagp, sb, k);
        at(d->realp, sd, k) = ar * br - ai * bi + at(c->realp, sc, k);
        at(d->imagp, sd, k) = ar * bi + ai * br + at(c->imagp, sc, k);
    }
}
void vDSP_zvabs_(const DSPSplitComplex *a, std::int64_t sa, float *c, std::int64_t sc, std::uint64_t n) DARWIN(vDSP_zvabs);
void vDSP_zvabs_(const DSPSplitComplex *a, std::int64_t sa, float *c, std::int64_t sc, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        const auto k = static_cast<std::int64_t>(i);
        at(c, sc, k) = std::hypot(at(a->realp, sa, k), at(a->imagp, sa, k));
    }
}
void vDSP_zvphas_(const DSPSplitComplex *a, std::int64_t sa, float *c, std::int64_t sc, std::uint64_t n) DARWIN(vDSP_zvphas);
void vDSP_zvphas_(const DSPSplitComplex *a, std::int64_t sa, float *c, std::int64_t sc, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; i++) {
        const auto k = static_cast<std::int64_t>(i);
        at(c, sc, k) = std::atan2(at(a->imagp, sa, k), at(a->realp, sa, k));
    }
}

/* ---- vForce: elementwise transcendentals ------------------------------------------- */

void vvexpf_(float *out, const float *in, const std::int32_t *n) DARWIN(vvexpf);
void vvexpf_(float *out, const float *in, const std::int32_t *n) {
    for (std::int32_t i = 0; i < *n; i++) out[i] = std::exp(in[i]);
}
void vvlogf_(float *out, const float *in, const std::int32_t *n) DARWIN(vvlogf);
void vvlogf_(float *out, const float *in, const std::int32_t *n) {
    for (std::int32_t i = 0; i < *n; i++) out[i] = std::log(in[i]);
}
void vvsincosf_(float *sines, float *cosines, const float *in, const std::int32_t *n) DARWIN(vvsincosf);
void vvsincosf_(float *sines, float *cosines, const float *in, const std::int32_t *n) {
    for (std::int32_t i = 0; i < *n; i++) {
        sines[i] = std::sin(in[i]);
        cosines[i] = std::cos(in[i]);
    }
}

__attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}

} // extern "C"
