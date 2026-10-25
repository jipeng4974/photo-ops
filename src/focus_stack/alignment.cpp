// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <opencv2/core.hpp>
#include <opencv2/features.hpp>
#include <opencv2/geometry.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>
module photo_ops.focus_stack;
import photo_ops.image;

namespace photo_ops::detail {
namespace {
cv::Mat feature_image(image_view<const gray> source) {
  float lo = source.at(0, 0).value, hi = lo;
  for (std::size_t y = 0; y < source.size().height; ++y)
    for (auto p : source.row(y)) {
      lo = std::min(lo, p.value);
      hi = std::max(hi, p.value);
    }
  cv::Mat result(source.size().height, source.size().width, CV_8UC1);
  const double scale = hi > lo ? 255. / (static_cast<double>(hi) - lo) : 0;
  for (std::size_t y = 0; y < source.size().height; ++y)
    for (std::size_t x = 0; x < source.size().width; ++x)
      result.at<unsigned char>(y, x) = std::clamp(
          std::lround((static_cast<double>(source.at(x, y).value) - lo) * scale), 0l, 255l);
  return result;
}

struct rng_restore {
  std::uint64_t state = cv::theRNG().state;

  ~rng_restore() {
    cv::theRNG().state = state;
  }
};
} // namespace

result<focus_stack_alignment> align_focus_stack(image_view<const gray> reference,
                                                image_view<const gray> source) {
  const auto size = reference.size();
  if (size.width < 16 || size.height < 16 || size.width > 2048 || size.height > 2048 ||
      size.width != source.size().width || size.height != source.size().height)
    return fail(error_code::invalid_argument, "focus registration",
                "need same-sized finite luminance proxies, 16..2048 pixels per side");
  bool equal = true;
  for (std::size_t y = 0; y < size.height; ++y)
    for (std::size_t x = 0; x < size.width; ++x) {
      const auto a = reference.at(x, y).value, b = source.at(x, y).value;
      if (!std::isfinite(a) || !std::isfinite(b))
        return fail(error_code::invalid_argument, "focus registration", "non-finite proxy");
      equal &= a == b;
    }
  if (equal)
    return focus_stack_alignment{};
  try {
    // OpenCV stays behind this adapter. Neither its pixels nor its geometry escape.
    const auto first = feature_image(reference), second = feature_image(source);
    std::vector<cv::KeyPoint> ref_points, src_points;
    cv::Mat ref_descriptors, src_descriptors;
    auto sift = cv::SIFT::create(2000);
    sift->detectAndCompute(first, cv::noArray(), ref_points, ref_descriptors);
    sift->detectAndCompute(second, cv::noArray(), src_points, src_descriptors);
    if (ref_descriptors.empty() || src_descriptors.empty())
      return fail(error_code::decode, "focus registration", "insufficient textured features");
    cv::BFMatcher matcher(cv::NORM_L2);
    std::vector<std::vector<cv::DMatch>> forward, reverse;
    matcher.knnMatch(src_descriptors, ref_descriptors, forward, 2);
    matcher.knnMatch(ref_descriptors, src_descriptors, reverse, 2);
    std::vector<cv::Point2f> from, to;
    for (const auto &pair : forward) {
      if (pair.size() != 2 || pair[0].distance >= .75f * pair[1].distance)
        continue;
      const auto &best = pair[0];
      const auto &back = reverse[best.trainIdx];
      if (back.size() != 2 || back[0].distance >= .75f * back[1].distance ||
          back[0].trainIdx != best.queryIdx)
        continue;
      from.push_back(src_points[best.queryIdx].pt);
      to.push_back(ref_points[best.trainIdx].pt);
    }
    if (from.size() < 8)
      return fail(error_code::decode, "focus registration", "fewer than eight mutual matches");
    rng_restore restore;
    cv::theRNG().state = 0x50484f544fULL;
    cv::Mat inliers;
    const auto matrix =
        cv::estimateAffinePartial2D(from, to, inliers, cv::RANSAC, 2., 2000, .995, 10);
    if (matrix.empty())
      return fail(error_code::decode, "focus registration", "similarity estimation failed");
    focus_stack_alignment result;
    result.a = matrix.at<double>(0, 0);
    result.b = matrix.at<double>(1, 0);
    result.tx = matrix.at<double>(0, 2);
    result.ty = matrix.at<double>(1, 2);
    result.matches = from.size();
    double squares = 0;
    for (std::size_t i = 0; i < from.size(); ++i)
      if (inliers.at<unsigned char>(i)) {
        const double dx = result.a * from[i].x - result.b * from[i].y + result.tx - to[i].x;
        const double dy = result.b * from[i].x + result.a * from[i].y + result.ty - to[i].y;
        squares += dx * dx + dy * dy;
        ++result.inliers;
      }
    const auto scale = std::hypot(result.a, result.b);
    if (result.inliers < 8 || result.inliers < .35 * result.matches || !std::isfinite(scale) ||
        scale < .9 || scale > 1.1 || std::abs(std::atan2(result.b, result.a)) > .1 ||
        std::hypot(result.tx, result.ty) > .25 * std::max(size.width, size.height))
      return fail(error_code::decode, "focus registration",
                  "weak consensus or excessive similarity deformation");
    result.residual_px = std::sqrt(squares / result.inliers);
    if (!std::isfinite(result.residual_px) || result.residual_px > 1.5)
      return fail(error_code::decode, "focus registration", "excessive alignment residual");
    result.identity = false;
    return result;
  } catch (const cv::Exception &e) {
    return fail(error_code::decode, "focus registration", e.what());
  }
}

namespace {
cv::Mat normalized_image(image_view<const gray> src, double sigma) {
  cv::Mat pixels(src.size().height, src.size().width, CV_32F);
  for (std::size_t y = 0; y < src.size().height; ++y)
    for (std::size_t x = 0; x < src.size().width; ++x)
      pixels.at<float>(y, x) = src.at(x, y).value;
  cv::GaussianBlur(pixels, pixels, {}, sigma, sigma, cv::BORDER_REPLICATE);
  cv::Scalar mean, deviation;
  cv::meanStdDev(pixels, mean, deviation);
  pixels = (pixels - mean[0]) / std::max(deviation[0], 1e-6);
  return pixels;
}

double sample(const cv::Mat &image, double x, double y) {
  const int ix = static_cast<int>(x), iy = static_cast<int>(y);
  const double fx = x - ix, fy = y - iy;
  return (1 - fy) * ((1 - fx) * image.at<float>(iy, ix) + fx * image.at<float>(iy, ix + 1)) +
         fy * ((1 - fx) * image.at<float>(iy + 1, ix) + fx * image.at<float>(iy + 1, ix + 1));
}

struct direct_evidence {
  cv::Matx44d hessian = cv::Matx44d::zeros();
  cv::Vec4d rhs{};
  double loss = 0, error = 0, gradient = 0, cross = 0, ref_square = 0, src_square = 0;
  std::size_t samples = 0;
};

direct_evidence evaluate(const cv::Mat &reference, const cv::Mat &source, const cv::Mat &gx,
                         const cv::Mat &gy, const focus_stack_alignment &t) {
  direct_evidence out;
  const double cx = (source.cols - 1) * .5, cy = (source.rows - 1) * .5;
  const double radius = std::max(source.cols, source.rows) * .5;
  const int stride = std::max(1, static_cast<int>(std::sqrt(source.total() / 250000.)));
  for (int y = 8; y < source.rows - 8; y += stride)
    for (int x = 8; x < source.cols - 8; x += stride) {
      const double rx = t.a * x - t.b * y + t.tx, ry = t.b * x + t.a * y + t.ty;
      if (rx < 8 || ry < 8 || rx >= reference.cols - 9 || ry >= reference.rows - 9)
        continue;
      const double dx = sample(gx, rx, ry), dy = sample(gy, rx, ry);
      if (dx * dx + dy * dy < .0004)
        continue;
      const double r = sample(reference, rx, ry), s = source.at<float>(y, x);
      const double error = r - s;
      // Huber weighting limits moving objects, occlusions and changing defocus evidence.
      const double weight = std::min(1., .15 / std::max(std::abs(error), 1e-12));
      const double u = (x - cx) / radius, v = (y - cy) / radius;
      const cv::Vec4d j(dx, dy, dx * u + dy * v, -dx * v + dy * u);
      out.hessian += weight * (j * j.t());
      out.rhs += weight * error * j;
      out.loss += std::abs(error) <= .15 ? .5 * error * error : .15 * (std::abs(error) - .075);
      out.error += weight * error * error;
      out.gradient += weight * (dx * dx + dy * dy);
      out.cross += r * s;
      out.ref_square += r * r;
      out.src_square += s * s;
      ++out.samples;
    }
  if (out.samples)
    out.loss /= out.samples;
  return out;
}
} // namespace

result<focus_stack_alignment> refine_focus_stack(image_view<const gray> reference,
                                                 image_view<const gray> source,
                                                 focus_stack_alignment initial) {
  try {
    auto t = initial;
    for (double sigma : {3., 1.5, .75}) {
      const auto ref = normalized_image(reference, sigma),
                 src = normalized_image(source, sigma);
      cv::Mat gx, gy;
      cv::Sobel(ref, gx, CV_32F, 1, 0, 3, 1. / 8, 0, cv::BORDER_REPLICATE);
      cv::Sobel(ref, gy, CV_32F, 0, 1, 3, 1. / 8, 0, cv::BORDER_REPLICATE);
      auto evidence = evaluate(ref, src, gx, gy, t);
      if (evidence.samples < 64)
        return fail(error_code::decode, "focus refinement",
                    "insufficient dense gradient evidence");
      for (int iteration = 0; iteration < 25; ++iteration) {
        cv::Vec4d delta;
        if (!cv::solve(evidence.hessian, -evidence.rhs, delta, cv::DECOMP_CHOLESKY))
          return fail(error_code::decode, "focus refinement",
                      "ill-conditioned similarity refinement");
        const double magnitude = cv::norm(delta);
        if (!std::isfinite(magnitude) || magnitude > 10)
          break;
        if (magnitude < 1e-5)
          break;
        bool improved = false;
        for (double rate : {1., .5, .25, .125}) {
          auto candidate = t;
          const double radius = std::max(src.cols, src.rows) * .5;
          const double da = rate * delta[2] / radius, db = rate * delta[3] / radius;
          candidate.a += da;
          candidate.b += db;
          candidate.tx += rate * delta[0] - da * (src.cols - 1) * .5 + db * (src.rows - 1) * .5;
          candidate.ty += rate * delta[1] - db * (src.cols - 1) * .5 - da * (src.rows - 1) * .5;
          auto next = evaluate(ref, src, gx, gy, candidate);
          if (next.samples >= evidence.samples * .95 && next.loss < evidence.loss) {
            t = candidate;
            evidence = next;
            improved = true;
            break;
          }
        }
        if (!improved)
          break;
      }
      t.refinement_samples = evidence.samples;
      t.dense_residual_px = std::sqrt(2 * evidence.error / std::max(evidence.gradient, 1e-12));
      t.dense_correlation =
          evidence.cross /
          std::sqrt(std::max(evidence.ref_square * evidence.src_square, 1e-12));
    }
    // Keep the feature consensus basin and the four-parameter geometry contract.
    const double radius = std::max(reference.size().width, reference.size().height);
    const double correction = std::hypot(t.tx - initial.tx, t.ty - initial.ty) +
                              radius * std::hypot(t.a - initial.a, t.b - initial.b);
    if (!std::isfinite(t.dense_residual_px) || t.dense_correlation < .75 || correction > 12 ||
        std::hypot(t.a, t.b) < .9 || std::hypot(t.a, t.b) > 1.1 ||
        std::abs(std::atan2(t.b, t.a)) > .1)
      return fail(error_code::decode, "focus refinement",
                  "inconsistent dense alignment or excessive correction");
    t.refined = true;
    return t;
  } catch (const cv::Exception &e) {
    return fail(error_code::decode, "focus refinement", e.what());
  }
}
} // namespace photo_ops::detail
