#include "frame_clock.h"
#include "render_pose.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <unordered_map>
#include <vector>

using namespace trials_hd;
using namespace std::chrono_literals;

void Check(bool condition) {
  if (!condition) { std::cerr << "render pose check failed\n"; std::exit(1); }
}
bool Near(float a, float b) { return std::abs(a - b) < 0.0001f; }
RenderMatrix Pose(float x, float angle = 0) {
  const float c = std::cos(angle), s = std::sin(angle);
  return {c,-s,0,0, s,c,0,0, 0,0,1,0, x,0,0,1};
}

int main() {
  const auto a = Pose(0), b = Pose(1, 1.57079632679f);
  Check(InterpolatePose(a,b,0) == a && InterpolatePose(a,b,1) == b);
  const auto middle = InterpolatePose(a,b,.5f);
  Check(Near(middle[12], .5f) && Near(middle[0], std::sqrt(.5f)));
  for (int i = 0; i < 3; ++i) {
    Check(Near(std::hypot(middle[4*i], middle[4*i+1], middle[4*i+2]), 1));
    for (int j = i+1; j < 3; ++j)
      Check(Near(middle[4*i]*middle[4*j]+middle[4*i+1]*middle[4*j+1]+middle[4*i+2]*middle[4*j+2], 0));
  }
  // Half-turns, wraparound, nonuniform scale and mirrored transforms retain geometry.
  for (float angle : {3.14159265f, -3.14159265f, 2.8f}) {
    auto scaled = Pose(1,angle);
    for (int j = 0; j < 3; ++j) { scaled[j] *= 2; scaled[4+j] *= 3; scaled[8+j] *= -4; }
    auto start = Pose(0);
    for (int j = 0; j < 3; ++j) { start[j] *= 2; start[4+j] *= 3; start[8+j] *= -4; }
    const auto blend = InterpolatePose(start,scaled,.5f);
    Check(Near(std::hypot(blend[0],blend[1],blend[2]),2));
    Check(Near(std::hypot(blend[4],blend[5],blend[6]),3));
    Check(Near(std::hypot(blend[8],blend[9],blend[10]),4));
  }
  auto reflected = a;
  reflected[10] = -1;
  Check(InterpolatePose(a,reflected,.5f) == reflected);
  const auto wrap = InterpolatePose(Pose(0,3.1f),Pose(0,-3.1f),.5f);
  Check(wrap[0] < -.999f); // Shortest rotation, not a full spin through zero.
  const auto identity = Multiply(b,InverseView(b));
  for (int i = 0; i < 16; ++i) Check(Near(identity[i], a[i]));
  auto sheared = a;
  sheared[1] = .5f;
  Check(Near(InterpolatePose(a,sheared,.5f)[1], .25f));
  auto projected = b;
  projected[11] = 1;
  Check(Near(InterpolatePose(a,projected,.5f)[11], .5f));
  RenderMatrix projection = {2,0,0,0, 0,3,0,0, 0,0,1,1, 0,0,-.1f,0};
  const auto view = InverseView(middle);
  const auto view_projection = Multiply(view,projection);
  Check(Near(view_projection[12], -middle[12] * view[0] * 2));
  Check(Near(view_projection[15], view[14]));
  const RenderMatrix inverse_projection = {.5f,0,0,0, 0,1.f/3,0,0, 0,0,0,-10, 0,0,1,10};
  const auto inverse_combined = Multiply(inverse_projection,middle);
  const auto combined_identity = Multiply(view_projection,inverse_combined);
  for (int i = 0; i < 16; ++i) Check(Near(combined_identity[i],a[i]));

  PoseHistory history;
  Check(history.Sample(10,Pose(0),.5f) == Pose(0));
  Check(Near(history.Sample(11,Pose(1),.25f)[12], .25f));
  Check(Near(history.Sample(11,Pose(1),.75f)[12], .75f)); // Repeated passes do not advance history.
  Check(history.Sample(14,Pose(1.5f),.5f) == Pose(1.5f)); // Missed simulation samples snap.
  Check(history.Sample(15,Pose(20),.5f) == Pose(20)); // Respawn/teleport, no sweep across the track.
  history = {};
  Check(history.Sample(16,Pose(-30),.5f) == Pose(-30)); // Scene change.
  std::unordered_map<uint64_t,PoseHistory> cameras;
  auto mirror = a;
  mirror[5] = -1;
  const auto main_key = CameraPoseKey(1,0,a,projection);
  const auto shadow_key = CameraPoseKey(1,1,a,projection);
  const auto mirror_key = CameraPoseKey(1,0,mirror,projection);
  Check(main_key != shadow_key && main_key != mirror_key && shadow_key != mirror_key);
  Check(main_key != CameraPoseKey(1,0,a,a)); // Orthographic and perspective reuse is separate.
  cameras[main_key].Sample(10,Pose(0),.5f);
  cameras[shadow_key].Sample(10,Pose(30),.5f);
  cameras[mirror_key].Sample(10,Pose(-30),.5f);
  Check(Near(cameras[main_key].Sample(11,Pose(1),.5f)[12], .5f));
  Check(Near(cameras[shadow_key].Sample(11,Pose(31),.5f)[12],30.5f));
  cameras[mirror_key].Sample(11,Pose(-29),.5f);
  Check(Near(cameras[main_key].Sample(11,Pose(1),.75f)[12],.75f)); // Other passes cannot reset the game camera.

  // Two views drawn through the same camera and pass each frame keep separate histories
  // (one shared history snapped every sample, so the camera never interpolated).
  PoseTracks views;
  for (uint32_t tick = 20; tick < 23; ++tick) {
    for (float alpha : {.25f, .75f}) {
      const float near_view = float(tick) * .03f, far_view = 7 + float(tick) * .01f;
      const auto first = views.Sample(tick, Pose(near_view), alpha);
      const auto second = views.Sample(tick, Pose(far_view, .5f), alpha);
      if (tick > 20) {
        Check(Near(first[12], near_view - .03f * (1 - alpha)));
        Check(Near(second[12], far_view - .01f * (1 - alpha)));
      }
    }
  }
  Check(views.Sample(23, Pose(40), .5f) == Pose(40)); // A new view or a cut snaps.
  Check(Near(views.Sample(23, Pose(.69f), .5f)[12], .675f)); // Existing views still blend.

  // A wheel spinning 55 degrees per tick about a hub 0.83 units from its matrix origin, while
  // flying along: its hub must stay on a straight path between ticks (blending the origin in a
  // straight line pulled the hub up to 0.06 units off its axle - an oval wobble).
  {
    const RenderPoint hub = {.1f, .35f, -.75f};
    auto wheel = [&](uint32_t tick) {
      const float angle = .96f * float(tick), c = std::cos(angle), s = std::sin(angle);
      RenderMatrix m = {1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1};  // Spin about the X axle.
      const auto spun = TransformPoint(m, hub);
      const RenderPoint at = {.15f * float(tick), 2 - .01f * float(tick * tick), 0};
      for (int j = 0; j < 3; ++j) m[12 + j] = at[j] - spun[j];
      return m;
    };
    PoseHistory spinning;
    float worst = 0;
    for (uint32_t tick = 0; tick < 30; ++tick) {
      for (float alpha : {.25f, .5f, .75f}) {
        const auto drawn = TransformPoint(spinning.Sample(tick, wheel(tick), alpha), hub);
        if (tick < 4) continue;
        const auto from = TransformPoint(wheel(tick - 1), hub), to = TransformPoint(wheel(tick), hub);
        for (int j = 0; j < 3; ++j) worst = std::max(worst, std::abs(drawn[j] - std::lerp(from[j], to[j], alpha)));
      }
    }
    Check(worst < .002f);
    // Transforms turning slowly about their own origin keep the origin as the pivot.
    PoseHistory turning;
    for (uint32_t tick = 0; tick < 30; ++tick) turning.Sample(tick, Pose(.05f * float(tick), .01f * float(tick)), .5f);
    Check(Near(turning.Sample(30, Pose(1.5f, .3f), .5f)[12], 1.475f));
  }

  // At 120/144/240 Hz, shader poses are distinct even though simulation stays at 60 Hz.
  for (int fps : {120,144,240}) {
    FrameClock clock;
    PoseHistory poses;
    uint32_t tick = 0;
    auto previous = 0ns;
    std::set<float> distinct;
    for (int frame = 1; frame <= fps * 2; ++frame) {
      const auto now = FrameClock::Duration(2s) * frame / (fps * 2);
      tick += clock.Advance(now-previous);
      Check(clock.InterpolationFraction() >= 0 && clock.InterpolationFraction() < 1);
      const auto simulation = Pose(float(tick) * .01f);
      const auto rendered = poses.Sample(tick,simulation,clock.InterpolationFraction());
      Check(rendered[12] <= simulation[12] && rendered[12] >= simulation[12] - .01001f);
      distinct.insert(rendered[12]);
      previous = now;
    }
    Check(tick == 120 && distinct.size() >= size_t(fps * 2 - 3));
  }

  // Renderer pointers, camera bytes and PPC stack are restored exactly, even on an early exit.
  std::vector<uint8_t> memory(16384);
  for (size_t i = 0; i < memory.size(); ++i) memory[i] = uint8_t(i * 37);
  const auto original = memory;
  uint32_t stack = 16000;
  {
    ScopedRenderUpload upload(memory.data(),128,2048,stack);
    Check(stack == 13920 && upload.scratch() % 16 == 0);
    Check(upload.scratch() + 30 * 64 <= 16000);
    std::fill_n(memory.data()+2048+48,12,0);
    std::fill_n(memory.data()+128+144,384,0);
    std::fill_n(memory.data()+128+944,120,0);
    std::fill_n(memory.data()+upload.scratch(),30*64,0);
  }
  Check(stack == 16000);
  Check(std::equal(original.begin(),original.begin()+13920,memory.begin()));
  std::cout << "render poses: rigid rotations, cadence, discontinuities and state restoration passed\n";
}
