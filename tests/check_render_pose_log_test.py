#!/usr/bin/env python3
import unittest
from check_render_pose_log import check


def sample(frame, count, rendered, camera_rendered, tick=100):
    return (f"render pose: frame={frame} tick={tick} alpha=0.500000 node=00ABCDEF "
            f"matrices={count} component=12 raw=1.0000000 rendered={rendered:.7f} "
            f"camera=00123456 camera_raw_x=5.0000000 camera_rendered_x={camera_rendered:.7f}\n")


class PoseLogTest(unittest.TestCase):
    def test_subtick_motion(self):
        log = ''.join(sample(f,n,p,c) for f,p,c in [(1,.25,4.25),(2,.75,4.75)] for n in [1,11])
        self.assertEqual(check(log), (1,1,1))

    def test_redrawing_fixed_poses_is_not_smooth(self):
        log = ''.join(sample(f,n,1,5) for f in [1,2] for n in [1,11])
        with self.assertRaises(AssertionError): check(log)

    def test_different_ticks_are_not_intermediate_frames(self):
        log = ''.join(sample(f,n,.5,4.5,tick=f) for f in [1,2] for n in [1,11])
        with self.assertRaises(AssertionError): check(log)

    def test_multiple_draws_in_one_frame_do_not_count(self):
        log = ''.join(sample(1,n,p,c) for p,c in [(.25,4.25),(.75,4.75)] for n in [1,11])
        with self.assertRaises(AssertionError): check(log)

    def test_camera_snapping_back_to_raw_does_not_count(self):
        log = ''.join(sample(f,n,p,c) for f,p,c in [(1,.25,4.25),(2,.75,5)] for n in [1,11])
        with self.assertRaises(AssertionError): check(log)

    def test_different_camera_views_do_not_count(self):
        log = ''.join(sample(f,n,p,c).replace('camera_raw_x=', f'camera_view={f:016X} camera_raw_x=')
                      for f,p,c in [(1,.25,4.25),(2,.75,4.75)] for n in [1,11])
        with self.assertRaises(AssertionError): check(log)

    def test_guest_fault_is_rejected(self):
        with self.assertRaises(AssertionError): check("Unhandled guest access violation")


if __name__ == '__main__': unittest.main()
