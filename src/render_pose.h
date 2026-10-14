#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace trials_hd {
using RenderMatrix = std::array<float, 16>;

inline RenderMatrix Multiply(const RenderMatrix& a, const RenderMatrix& b) {
  RenderMatrix out{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      for (int k = 0; k < 4; ++k) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
  return out;
}

inline uint64_t CameraPoseKey(uint32_t camera, uint32_t pass,
                              const RenderMatrix& view, const RenderMatrix& projection) {
  const float det = view[0] * (view[5] * view[10] - view[6] * view[9]) -
                    view[1] * (view[4] * view[10] - view[6] * view[8]) +
                    view[2] * (view[4] * view[9] - view[5] * view[8]);
  // A camera object may be reused for shadows and reflected views in the same frame.
  return (uint64_t(camera) << 32) | uint32_t(pass << 2) |
         (projection[11] == 0 ? 0u : 1u) | (det < 0 ? 2u : 0u);
}

inline RenderMatrix InverseView(const RenderMatrix& m) {
  RenderMatrix out{};
  out[15] = 1;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) out[r * 4 + c] = m[c * 4 + r];
  for (int c = 0; c < 3; ++c)
    for (int k = 0; k < 3; ++k) out[12 + c] -= m[12 + k] * out[k * 4 + c];
  return out;
}

inline bool Rotation(const RenderMatrix& m, std::array<float, 4>& q,
                     std::array<float, 3>& scale) {
  if (std::abs(m[3]) > 1e-6f || std::abs(m[7]) > 1e-6f ||
      std::abs(m[11]) > 1e-6f || std::abs(m[15] - 1) > 1e-6f) return false;
  float r[3][3];
  for (int i = 0; i < 3; ++i) {
    scale[i] = std::hypot(m[i * 4], m[i * 4 + 1], m[i * 4 + 2]);
    if (!std::isfinite(scale[i]) || scale[i] < 1e-6f) return false;
    for (int j = 0; j < 3; ++j) r[i][j] = m[i * 4 + j] / scale[i];
  }
  for (int i = 0; i < 3; ++i)
    for (int j = i + 1; j < 3; ++j)
      if (std::abs(r[i][0] * r[j][0] + r[i][1] * r[j][1] + r[i][2] * r[j][2]) > .001f)
        return false;
  const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                    r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                    r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  if (det < 0) {
    scale[2] = -scale[2];
    for (auto& x : r[2]) x = -x;
  }
  const float trace = r[0][0] + r[1][1] + r[2][2];
  if (trace > 0) {
    const float s = 2 * std::sqrt(trace + 1);
    q = {(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s,
         (r[1][0] - r[0][1]) / s, s / 4};
  } else {
    int i = r[1][1] > r[0][0] ? 1 : 0;
    if (r[2][2] > r[i][i]) i = 2;
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const float s = 2 * std::sqrt(std::max(0.f, 1 + r[i][i] - r[j][j] - r[k][k]));
    if (s < 1e-6f) return false;
    q[i] = s / 4;
    q[j] = (r[j][i] + r[i][j]) / s;
    q[k] = (r[k][i] + r[i][k]) / s;
    q[3] = (r[k][j] - r[j][k]) / s;
  }
  return true;
}

inline RenderMatrix InterpolatePose(const RenderMatrix& a, const RenderMatrix& b, float alpha) {
  if (alpha <= 0) return a;
  if (alpha >= 1 || a == b) return b;
  RenderMatrix out;
  for (int i = 0; i < 16; ++i) out[i] = std::lerp(a[i], b[i], alpha);
  std::array<float, 4> qa{}, qb{};
  std::array<float, 3> sa{}, sb{};
  // shortcut: sheared transforms use matrix lerp; add polar decomposition if visible in assets.
  if (!Rotation(a, qa, sa) || !Rotation(b, qb, sb)) return out;
  for (int i = 0; i < 3; ++i) if (sa[i] * sb[i] <= 0) return b;
  float dot = 0;
  for (int i = 0; i < 4; ++i) dot += qa[i] * qb[i];
  float norm = 0;
  for (int i = 0; i < 4; ++i) {
    qa[i] = std::lerp(qa[i], dot < 0 ? -qb[i] : qb[i], alpha);
    norm += qa[i] * qa[i];
  }
  if (norm < 1e-12f) return out;
  for (auto& x : qa) x /= std::sqrt(norm);
  const auto [x, y, z, w] = qa;
  const float r[3][3] = {{1 - 2 * (y*y + z*z), 2 * (x*y - z*w), 2 * (x*z + y*w)},
                         {2 * (x*y + z*w), 1 - 2 * (x*x + z*z), 2 * (y*z - x*w)},
                         {2 * (x*z - y*w), 2 * (y*z + x*w), 1 - 2 * (x*x + y*y)}};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out[i * 4 + j] = r[i][j] * std::lerp(sa[i], sb[i], alpha);
  return out;
}

// Larger changes between ticks are teleports (respawns, cuts) and snap.
constexpr float kPoseSnapDistance = 2.f;

class PoseHistory {
 public:
  RenderMatrix Sample(uint32_t tick, const RenderMatrix& pose, float alpha) {
    // shortcut: >2-unit jumps snap as teleports; use explicit reset signals to blend faster debris.
    if (!valid_ || (tick != tick_ && tick != tick_ + 1) ||
        std::hypot(pose[12] - current_[12], pose[13] - current_[13],
                   pose[14] - current_[14]) > kPoseSnapDistance) {
      previous_ = pose;
    } else if (tick != tick_) {
      previous_ = current_;
    }
    current_ = pose;
    tick_ = tick;
    valid_ = true;
    return InterpolatePose(previous_, current_, alpha);
  }
  uint32_t tick() const { return tick_; }
  bool valid() const { return valid_; }
  // How far a pose is from the latest sample: translation plus rotation/scale change.
  float Distance(const RenderMatrix& pose) const {
    float basis = 0;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) basis += std::abs(pose[r * 4 + c] - current_[r * 4 + c]);
    return std::hypot(pose[12] - current_[12], pose[13] - current_[13], pose[14] - current_[14]) +
           basis;
  }
 private:
  RenderMatrix previous_{}, current_{};
  uint32_t tick_ = 0;
  bool valid_ = false;
};

// One camera object can render several views in a frame (Trials HD draws the
// track with two different positions and fields of view through the same
// camera and pass). Keep a history per view, matched to the nearest latest pose,
// so the views don't keep resetting each other's history as teleports.
class PoseTracks {
 public:
  RenderMatrix Sample(uint32_t tick, const RenderMatrix& pose, float alpha) {
    PoseHistory* best = nullptr;
    float best_distance = kPoseSnapDistance;
    for (auto& track : tracks_) {
      if (!track.valid() || uint32_t(tick - track.tick()) > 1) continue;
      const float distance = track.Distance(pose);
      if (distance < best_distance) {
        best = &track;
        best_distance = distance;
      }
    }
    if (!best) {
      // A new view: reuse a free or the least recently sampled track.
      best = &tracks_[0];
      for (auto& track : tracks_) {
        if (!track.valid()) {
          best = &track;
          break;
        }
        if (uint32_t(tick - track.tick()) > uint32_t(tick - best->tick())) best = &track;
      }
      *best = {};
    }
    return best->Sample(tick, pose, alpha);
  }
 private:
  std::array<PoseHistory, 4> tracks_{};
};

// Restore graphics-camera/renderer inputs; model matrices only live in guest-stack scratch.
class ScopedRenderUpload {
 public:
  ScopedRenderUpload(uint8_t* base, uint32_t renderer, uint32_t camera, uint32_t& stack)
      : base_(base), renderer_(renderer), camera_(camera), stack_(stack), saved_stack_(stack) {
    std::memcpy(camera_position_.data(), base + camera + 48, camera_position_.size());
    std::memcpy(view_.data(), base + renderer + 144, view_.size());
    std::memcpy(palette_.data(), base + renderer + 944, palette_.size());
    stack -= 2080;
    for (int i = 0; i < 4; ++i) base[stack + i] = uint8_t(saved_stack_ >> (24 - i * 8));
  }
  ~ScopedRenderUpload() {
    std::memcpy(base_ + camera_ + 48, camera_position_.data(), camera_position_.size());
    std::memcpy(base_ + renderer_ + 144, view_.data(), view_.size());
    std::memcpy(base_ + renderer_ + 944, palette_.data(), palette_.size());
    stack_ = saved_stack_;
  }
  ScopedRenderUpload(const ScopedRenderUpload&) = delete;
  ScopedRenderUpload& operator=(const ScopedRenderUpload&) = delete;
  uint32_t scratch() const { return saved_stack_ - 2080 + 80; }
 private:
  uint8_t* base_;
  uint32_t renderer_, camera_;
  uint32_t& stack_;
  uint32_t saved_stack_;
  std::array<uint8_t, 12> camera_position_;
  std::array<uint8_t, 384> view_;
  std::array<uint8_t, 120> palette_;
};
}  // namespace trials_hd
