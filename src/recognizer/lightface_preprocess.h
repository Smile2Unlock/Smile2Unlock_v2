#pragma once
// Pre/post-processing for the LightFacePipeline ONNX models.
//
// Pure C++ (no ONNX Runtime, no modules) so the backend and the unit tests
// share it. Every function mirrors ../LightFacePipeline/common/{contracts,
// align,nms}.py: RGB colour order, 640x640 letterbox (pad 114), ArcFace 112
// Umeyama alignment, dual-scale (1.5x / 2.7x) 128x128 liveness crops.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace su::recognizer::lightface {

// --- Contracts (common/contracts.py, artifacts/manifest.json) -------------------

inline constexpr int kDetInputSize = 640;
inline constexpr float kDetScoreThreshold = 0.45F;
inline constexpr float kDetNmsIouThreshold = 0.45F;
inline constexpr std::uint8_t kDetPadValue = 114;
// Candidates kept before NMS: bounds the O(n^2) suppression on noisy frames.
inline constexpr std::size_t kDetMaxCandidates = 256;
// A box smaller than this cannot hold a usable face.
inline constexpr float kMinFaceSidePx = 16.0F;

inline constexpr int kRecInputSize = 112;
inline constexpr std::size_t kRecEmbeddingDim = 512;

inline constexpr std::size_t kFasClipLen = 8;
inline constexpr int kFasCropSize = 128;
inline constexpr std::size_t kFasFeatDim = 192;
inline constexpr double kFasScaleTexture = 1.5;
inline constexpr double kFasScaleContext = 2.7;
// Frames/clips blurrier than this carry no liveness verdict.
inline constexpr float kFasClarityThreshold = 0.35F;

// common/contracts.py REC_SAFE_LUMA_FLOOR_255: below this mean luma of the
// untouched face a face found only on the enhanced copy is not trusted.
inline constexpr double kFaceMeanLumaFloor = 25.0;

using Point = std::array<double, 2>;
using Points5 = std::array<Point, 5>;
using Landmarks = std::array<std::array<float, 2>, 5>;
// Row-major 2x3 matrix [a b tx; c d ty] mapping source -> destination.
using Affine = std::array<double, 6>;

inline constexpr Points5 kArcFaceDst112{{
    {38.2946, 51.6963},
    {73.5318, 51.5014},
    {56.0252, 71.7366},
    {41.5493, 92.3655},
    {70.7299, 92.2041},
}};

struct Detection {
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    float score = 0.0F;
    Landmarks kpts{};

    [[nodiscard]] float width() const { return x2 - x1; }
    [[nodiscard]] float height() const { return y2 - y1; }
};

// --- Resampling ------------------------------------------------------------------

namespace detail {

struct AxisTap {
    int first = 0;
    int count = 0;
    std::size_t weights = 0;  // offset into the weight pool
};

struct AxisPlan {
    std::vector<AxisTap> taps;
    std::vector<float> pool;
};

// area=true: fractional box average (cv2.INTER_AREA, used when shrinking).
// area=false: half-pixel-centre bilinear (cv2.INTER_LINEAR).
[[nodiscard]] inline AxisPlan plan_axis(const int src_n, const int dst_n, const bool area) {
    auto plan = AxisPlan{};
    plan.taps.reserve(static_cast<std::size_t>(dst_n));
    const auto scale = static_cast<double>(src_n) / static_cast<double>(dst_n);
    for (auto i = 0; i < dst_n; ++i) {
        auto tap = AxisTap{.weights = plan.pool.size()};
        if (area) {
            const auto lo = static_cast<double>(i) * scale;
            const auto hi = lo + scale;
            const auto first = std::clamp(static_cast<int>(std::floor(lo)), 0, src_n - 1);
            const auto last = std::clamp(static_cast<int>(std::ceil(hi)) - 1, first, src_n - 1);
            auto total = 0.0;
            for (auto k = first; k <= last; ++k) {
                const auto weight = std::max(0.0, std::min(static_cast<double>(k + 1), hi)
                    - std::max(static_cast<double>(k), lo));
                plan.pool.push_back(static_cast<float>(weight));
                total += weight;
            }
            for (auto k = 0; k <= last - first; ++k) {
                plan.pool[tap.weights + static_cast<std::size_t>(k)] /= static_cast<float>(
                    total > 0.0 ? total : 1.0);
            }
            tap.first = first;
            tap.count = last - first + 1;
        } else {
            const auto position = (static_cast<double>(i) + 0.5) * scale - 0.5;
            const auto base = static_cast<int>(std::floor(position));
            const auto fraction = static_cast<float>(position - base);
            const auto first = std::clamp(base, 0, src_n - 1);
            const auto second = std::clamp(base + 1, 0, src_n - 1);
            if (first == second) {
                plan.pool.push_back(1.0F);
                tap.first = first;
                tap.count = 1;
            } else {
                plan.pool.push_back(1.0F - fraction);
                plan.pool.push_back(fraction);
                tap.first = first;
                tap.count = 2;
            }
        }
        plan.taps.push_back(tap);
    }
    return plan;
}

[[nodiscard]] inline std::uint8_t to_u8(const float value) {
    return static_cast<std::uint8_t>(std::clamp(value + 0.5F, 0.0F, 255.0F));
}

}  // namespace detail

// Separable resize of a packed RGB8 image.
[[nodiscard]] inline std::vector<std::uint8_t> resize_rgb(
    const std::span<const std::uint8_t> src, const int src_w, const int src_h,
    const int dst_w, const int dst_h, const bool area) {
    if (src_w == dst_w && src_h == dst_h) {
        return {src.begin(), src.begin() + static_cast<std::ptrdiff_t>(
            static_cast<std::size_t>(src_w) * static_cast<std::size_t>(src_h) * 3)};
    }
    const auto columns = detail::plan_axis(src_w, dst_w, area);
    const auto rows = detail::plan_axis(src_h, dst_h, area);
    const auto dw = static_cast<std::size_t>(dst_w);
    // Horizontal pass: src_h rows of dst_w pixels, kept in float.
    auto middle = std::vector<float>(static_cast<std::size_t>(src_h) * dw * 3);
    for (auto y = 0; y < src_h; ++y) {
        const auto* row = src.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(src_w) * 3;
        auto* out = middle.data() + static_cast<std::size_t>(y) * dw * 3;
        for (auto x = 0; x < dst_w; ++x) {
            const auto& tap = columns.taps[static_cast<std::size_t>(x)];
            auto r = 0.0F;
            auto g = 0.0F;
            auto b = 0.0F;
            for (auto k = 0; k < tap.count; ++k) {
                const auto weight = columns.pool[tap.weights + static_cast<std::size_t>(k)];
                const auto* pixel = row + static_cast<std::size_t>(tap.first + k) * 3;
                r += weight * pixel[0];
                g += weight * pixel[1];
                b += weight * pixel[2];
            }
            out[x * 3 + 0] = r;
            out[x * 3 + 1] = g;
            out[x * 3 + 2] = b;
        }
    }
    auto result = std::vector<std::uint8_t>(dw * static_cast<std::size_t>(dst_h) * 3);
    for (auto y = 0; y < dst_h; ++y) {
        const auto& tap = rows.taps[static_cast<std::size_t>(y)];
        auto* out = result.data() + static_cast<std::size_t>(y) * dw * 3;
        for (auto x = 0; x < static_cast<int>(dw) * 3; ++x) {
            auto sum = 0.0F;
            for (auto k = 0; k < tap.count; ++k) {
                sum += rows.pool[tap.weights + static_cast<std::size_t>(k)]
                    * middle[static_cast<std::size_t>(tap.first + k) * dw * 3 + static_cast<std::size_t>(x)];
            }
            out[x] = detail::to_u8(sum);
        }
    }
    return result;
}

// --- Letterbox ---------------------------------------------------------------------

struct Letterbox {
    std::vector<std::uint8_t> rgb;  // target x target x 3
    float scale = 1.0F;
    int pad_x = 0;
    int pad_y = 0;
};

// common/align.py letterbox_rgb: aspect-preserving resize + constant padding.
[[nodiscard]] inline Letterbox letterbox_rgb(const std::span<const std::uint8_t> rgb, const int width,
    const int height, const int target = kDetInputSize, const std::uint8_t pad = kDetPadValue) {
    const auto ratio = std::min(static_cast<double>(target) / height, static_cast<double>(target) / width);
    // Python's round() is half-to-even, which is also the default FP rounding mode.
    const auto new_w = std::clamp(static_cast<int>(std::nearbyint(width * ratio)), 1, target);
    const auto new_h = std::clamp(static_cast<int>(std::nearbyint(height * ratio)), 1, target);
    const auto resized = resize_rgb(rgb, width, height, new_w, new_h, ratio < 1.0);
    auto result = Letterbox{};
    result.scale = static_cast<float>(ratio);
    result.pad_x = (target - new_w) / 2;
    result.pad_y = (target - new_h) / 2;
    result.rgb.assign(static_cast<std::size_t>(target) * static_cast<std::size_t>(target) * 3, pad);
    for (auto y = 0; y < new_h; ++y) {
        const auto* from = resized.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(new_w) * 3;
        auto* to = result.rgb.data()
            + (static_cast<std::size_t>(y + result.pad_y) * static_cast<std::size_t>(target)
               + static_cast<std::size_t>(result.pad_x)) * 3;
        std::copy_n(from, static_cast<std::size_t>(new_w) * 3, to);
    }
    return result;
}

// HWC uint8 -> CHW float32: out = value * scale + offset.
inline void pack_nchw(const std::span<const std::uint8_t> hwc, const int width, const int height,
    const float scale, const float offset, const std::span<float> out) {
    const auto plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (auto i = std::size_t{}; i < plane; ++i) {
        for (auto c = std::size_t{}; c < 3; ++c) {
            out[c * plane + i] = static_cast<float>(hwc[i * 3 + c]) * scale + offset;
        }
    }
}

// Detector input: RGB in [0, 1].
inline void pack_detector_input(const Letterbox& letterbox, const std::span<float> out) {
    pack_nchw(letterbox.rgb, kDetInputSize, kDetInputSize, 1.0F / 255.0F, 0.0F, out);
}

// Recognizer input: (rgb/255 - 0.5) / 0.5.
inline void pack_recognizer_input(const std::span<const std::uint8_t> aligned, const std::span<float> out) {
    pack_nchw(aligned, kRecInputSize, kRecInputSize, 1.0F / 127.5F, -1.0F, out);
}

// Liveness input: RGB in [0, 1].
inline void pack_fas_input(const std::span<const std::uint8_t> patch, const std::span<float> out) {
    pack_nchw(patch, kFasCropSize, kFasCropSize, 1.0F / 255.0F, 0.0F, out);
}

// --- Detections ------------------------------------------------------------------------

[[nodiscard]] inline float box_iou(const Detection& a, const Detection& b) {
    const auto area = [](const Detection& d) {
        return std::max(0.0F, d.x2 - d.x1) * std::max(0.0F, d.y2 - d.y1);
    };
    const auto iw = std::max(0.0F, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
    const auto ih = std::max(0.0F, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    const auto inter = iw * ih;
    return inter / std::max(area(a) + area(b) - inter, 1e-7F);
}

// Score filter + greedy NMS (common/nms.py). `boxes` is [N,4] xyxy, `scores`
// [N] and `kpts` [N,5,2], all in letterboxed pixel coordinates. Output is
// sorted by descending score.
[[nodiscard]] inline std::vector<Detection> decode_and_nms(const std::span<const float> boxes,
    const std::span<const float> scores, const std::span<const float> kpts,
    const float score_threshold = kDetScoreThreshold, const float iou_threshold = kDetNmsIouThreshold,
    const std::size_t max_candidates = kDetMaxCandidates) {
    const auto count = scores.size();
    if (boxes.size() < count * 4 || kpts.size() < count * 10) {
        return {};
    }
    auto candidates = std::vector<std::size_t>{};
    for (auto i = std::size_t{}; i < count; ++i) {
        if (std::isfinite(scores[i]) && scores[i] >= score_threshold) {
            candidates.push_back(i);
        }
    }
    const auto by_score = [&](const std::size_t a, const std::size_t b) {
        return scores[a] != scores[b] ? scores[a] > scores[b] : a < b;
    };
    if (candidates.size() > max_candidates) {
        std::ranges::partial_sort(candidates, candidates.begin() + static_cast<std::ptrdiff_t>(max_candidates), by_score);
        candidates.resize(max_candidates);
    } else {
        std::ranges::sort(candidates, by_score);
    }
    auto kept = std::vector<Detection>{};
    for (const auto index : candidates) {
        auto detection = Detection{
            .x1 = boxes[index * 4], .y1 = boxes[index * 4 + 1],
            .x2 = boxes[index * 4 + 2], .y2 = boxes[index * 4 + 3],
            .score = scores[index],
        };
        auto finite = std::isfinite(detection.x1) && std::isfinite(detection.y1)
            && std::isfinite(detection.x2) && std::isfinite(detection.y2);
        for (auto p = std::size_t{}; p < 5; ++p) {
            detection.kpts[p] = {kpts[index * 10 + p * 2], kpts[index * 10 + p * 2 + 1]};
            finite = finite && std::isfinite(detection.kpts[p][0]) && std::isfinite(detection.kpts[p][1]);
        }
        if (!finite) {
            continue;
        }
        const auto suppressed = std::ranges::any_of(kept, [&](const Detection& other) {
            return box_iou(detection, other) > iou_threshold;
        });
        if (!suppressed) {
            kept.push_back(detection);
        }
    }
    return kept;
}

// common/align.py unletterbox_coords: boxes are clipped to the original
// frame, landmarks are not.
[[nodiscard]] inline Detection unletterbox_detection(Detection detection, const float scale,
    const int pad_x, const int pad_y, const int original_w, const int original_h) {
    const auto inverse = 1.0F / std::max(scale, 1e-12F);
    const auto map_x = [&](const float v) { return (v - static_cast<float>(pad_x)) * inverse; };
    const auto map_y = [&](const float v) { return (v - static_cast<float>(pad_y)) * inverse; };
    detection.x1 = std::clamp(map_x(detection.x1), 0.0F, static_cast<float>(original_w));
    detection.x2 = std::clamp(map_x(detection.x2), 0.0F, static_cast<float>(original_w));
    detection.y1 = std::clamp(map_y(detection.y1), 0.0F, static_cast<float>(original_h));
    detection.y2 = std::clamp(map_y(detection.y2), 0.0F, static_cast<float>(original_h));
    for (auto& point : detection.kpts) {
        point = {map_x(point[0]), map_y(point[1])};
    }
    return detection;
}

// --- Alignment -----------------------------------------------------------------------------

namespace detail {

struct Svd2 {
    std::array<double, 4> u{};   // row-major
    std::array<double, 2> s{};
    std::array<double, 4> vt{};  // row-major
};

[[nodiscard]] inline std::array<double, 4> rotation(const double angle) {
    return {std::cos(angle), -std::sin(angle), std::sin(angle), std::cos(angle)};
}

[[nodiscard]] inline std::array<double, 4> mul2(const std::array<double, 4>& a, const std::array<double, 4>& b) {
    return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3],
            a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3]};
}

[[nodiscard]] inline double det2(const std::array<double, 4>& m) {
    return m[0] * m[3] - m[1] * m[2];
}

// Closed-form SVD of a 2x2 matrix: A = u * diag(s) * vt, s0 >= s1 >= 0.
[[nodiscard]] inline Svd2 svd2(const std::array<double, 4>& a) {
    const auto e = (a[0] + a[3]) / 2.0;
    const auto f = (a[0] - a[3]) / 2.0;
    const auto g = (a[2] + a[1]) / 2.0;
    const auto h = (a[2] - a[1]) / 2.0;
    const auto q = std::hypot(e, h);
    const auto r = std::hypot(f, g);
    const auto a1 = std::atan2(g, f);
    const auto a2 = std::atan2(h, e);
    auto result = Svd2{};
    result.u = rotation((a2 + a1) / 2.0);
    result.vt = rotation((a2 - a1) / 2.0);
    result.s = {q + r, q - r};
    if (result.s[1] < 0.0) {
        result.s[1] = -result.s[1];
        result.u[1] = -result.u[1];
        result.u[3] = -result.u[3];
    }
    return result;
}

}  // namespace detail

// common/align.py estimate_umeyama_similarity: rotation + uniform scale +
// translation mapping `src` onto `dst`, always a proper rotation (no mirror).
[[nodiscard]] inline Affine estimate_umeyama_similarity(const Points5& src, const Points5& dst) {
    constexpr auto kCount = 5.0;
    auto src_mean = Point{};
    auto dst_mean = Point{};
    for (auto i = std::size_t{}; i < 5; ++i) {
        for (auto c = std::size_t{}; c < 2; ++c) {
            src_mean[c] += src[i][c] / kCount;
            dst_mean[c] += dst[i][c] / kCount;
        }
    }
    auto cov = std::array<double, 4>{};  // dst_demean^T @ src_demean / n
    auto var_src = 0.0;
    for (auto i = std::size_t{}; i < 5; ++i) {
        const auto sx = src[i][0] - src_mean[0];
        const auto sy = src[i][1] - src_mean[1];
        const auto dx = dst[i][0] - dst_mean[0];
        const auto dy = dst[i][1] - dst_mean[1];
        cov[0] += dx * sx / kCount;
        cov[1] += dx * sy / kCount;
        cov[2] += dy * sx / kCount;
        cov[3] += dy * sy / kCount;
        var_src += (sx * sx + sy * sy) / kCount;
    }
    auto d = std::array<double, 2>{1.0, 1.0};
    if (detail::det2(cov) < 0.0) {
        d[1] = -1.0;
    }
    const auto svd = detail::svd2(cov);
    // numpy matrix_rank tolerance: s_max * max(M, N) * eps.
    const auto tolerance = svd.s[0] * 2.0 * std::numeric_limits<double>::epsilon();
    const auto rank = static_cast<int>(svd.s[0] > tolerance) + static_cast<int>(svd.s[1] > tolerance);
    if (rank == 0) {
        return {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    }
    auto linear = std::array<double, 4>{};
    const auto with_sign = [&](const double second) {
        return detail::mul2(detail::mul2(svd.u, {1.0, 0.0, 0.0, second}), svd.vt);
    };
    if (rank == 1) {
        linear = detail::det2(svd.u) * detail::det2(svd.vt) > 0.0 ? with_sign(1.0) : with_sign(-1.0);
    } else {
        linear = with_sign(d[1]);
    }
    const auto scale = (svd.s[0] * d[0] + svd.s[1] * d[1]) / std::max(var_src, 1e-12);
    for (auto& value : linear) {
        value *= scale;
    }
    const auto tx = dst_mean[0] - (linear[0] * src_mean[0] + linear[1] * src_mean[1]);
    const auto ty = dst_mean[1] - (linear[2] * src_mean[0] + linear[3] * src_mean[1]);
    return {linear[0], linear[1], tx, linear[2], linear[3], ty};
}

[[nodiscard]] inline Points5 to_points(const Landmarks& landmarks) {
    auto points = Points5{};
    for (auto i = std::size_t{}; i < 5; ++i) {
        points[i] = {landmarks[i][0], landmarks[i][1]};
    }
    return points;
}

// cv2.warpAffine(INTER_LINEAR, BORDER_CONSTANT, 0): `forward` maps source to
// destination coordinates (pixel centres on integers); taps outside the source
// read as black.
[[nodiscard]] inline std::vector<std::uint8_t> warp_affine_rgb(const std::span<const std::uint8_t> src,
    const int src_w, const int src_h, const Affine& forward, const int dst_w, const int dst_h) {
    auto result = std::vector<std::uint8_t>(
        static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h) * 3, 0);
    const auto det = forward[0] * forward[4] - forward[1] * forward[3];
    if (!(std::abs(det) > 1e-12)) {
        return result;
    }
    const auto ia = forward[4] / det;
    const auto ib = -forward[1] / det;
    const auto ic = -forward[3] / det;
    const auto id = forward[0] / det;
    const auto itx = -(ia * forward[2] + ib * forward[5]);
    const auto ity = -(ic * forward[2] + id * forward[5]);
    const auto tap = [&](const int x, const int y, const int c) -> float {
        if (x < 0 || y < 0 || x >= src_w || y >= src_h) {
            return 0.0F;
        }
        return static_cast<float>(src[(static_cast<std::size_t>(y) * static_cast<std::size_t>(src_w)
            + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)]);
    };
    for (auto y = 0; y < dst_h; ++y) {
        for (auto x = 0; x < dst_w; ++x) {
            const auto sx = ia * x + ib * y + itx;
            const auto sy = ic * x + id * y + ity;
            const auto x0 = static_cast<int>(std::floor(sx));
            const auto y0 = static_cast<int>(std::floor(sy));
            const auto fx = static_cast<float>(sx - x0);
            const auto fy = static_cast<float>(sy - y0);
            for (auto c = 0; c < 3; ++c) {
                const auto top = tap(x0, y0, c) * (1.0F - fx) + tap(x0 + 1, y0, c) * fx;
                const auto bottom = tap(x0, y0 + 1, c) * (1.0F - fx) + tap(x0 + 1, y0 + 1, c) * fx;
                result[(static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_w)
                    + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)]
                    = detail::to_u8(top * (1.0F - fy) + bottom * fy);
            }
        }
    }
    return result;
}

// 112x112 RGB crop aligned to the ArcFace template from 5 landmarks.
[[nodiscard]] inline std::vector<std::uint8_t> align_face_112(const std::span<const std::uint8_t> rgb,
    const int width, const int height, const Landmarks& landmarks) {
    const auto matrix = estimate_umeyama_similarity(to_points(landmarks), kArcFaceDst112);
    return warp_affine_rgb(rgb, width, height, matrix, kRecInputSize, kRecInputSize);
}

// common/align.py crop_square_scale: square patch centred on the box with side
// scale * max(w, h), warped to out_size.
[[nodiscard]] inline Affine square_crop_matrix(const float x1, const float y1, const float x2,
    const float y2, const double scale, const int out_size = kFasCropSize) {
    const auto bw = std::max(static_cast<double>(x2 - x1), 1.0);
    const auto bh = std::max(static_cast<double>(y2 - y1), 1.0);
    const auto cx = 0.5 * (static_cast<double>(x1) + x2);
    const auto cy = 0.5 * (static_cast<double>(y1) + y2);
    const auto crop_side = std::max(std::max(bw, bh) * scale, 2.0);
    const auto half = 0.5 * crop_side;
    const auto factor = static_cast<double>(out_size) / crop_side;
    return {factor, 0.0, -(cx - half) * factor, 0.0, factor, -(cy - half) * factor};
}

[[nodiscard]] inline std::vector<std::uint8_t> crop_square_scale(const std::span<const std::uint8_t> rgb,
    const int width, const int height, const Detection& face, const double scale,
    const int out_size = kFasCropSize) {
    return warp_affine_rgb(rgb, width, height,
        square_crop_matrix(face.x1, face.y1, face.x2, face.y2, scale, out_size), out_size, out_size);
}

struct DualCrop {
    std::vector<std::uint8_t> texture;  // 1.5x
    std::vector<std::uint8_t> context;  // 2.7x
};

[[nodiscard]] inline DualCrop crop_dual_scale_fas(const std::span<const std::uint8_t> rgb,
    const int width, const int height, const Detection& face) {
    return {crop_square_scale(rgb, width, height, face, kFasScaleTexture),
            crop_square_scale(rgb, width, height, face, kFasScaleContext)};
}

}  // namespace su::recognizer::lightface
