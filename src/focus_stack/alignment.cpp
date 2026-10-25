// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <opencv2/core.hpp>
#include <opencv2/features.hpp>
#include <opencv2/geometry.hpp>
#include <vector>
module photo_ops.focus_stack;
import photo_ops.image;

namespace photo_ops {
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
} // namespace photo_ops
