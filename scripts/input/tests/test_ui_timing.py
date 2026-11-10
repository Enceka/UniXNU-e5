#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the actual Settings input branch against a blocking diagnostic sink."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'E5HidDevice.cpp')
parser.add_argument('--expect-old', action='store_true')
args = parser.parse_args()
source = args.source.resolve()
text = source.read_text()
start = text.index('    if(touchTestKind==4) {', text.index('  void testTouchStep()'))
brace = text.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
body = text[start:end]
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "E5InputProtocol.h"
static uint64_t nowMs=0;
static bool busy=false,down=false;
static unsigned frames=10,rgb=100000,ink=20000;
static std::vector<uint64_t> events;
static uint64_t mach_absolute_time(){return nowMs;}
static void clock_interval_to_deadline(unsigned value,unsigned scale,uint64_t *out){*out=nowMs+uint64_t(value)*scale/1000000;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *out){*out=value*1000000;}
static unsigned e5_input_presented_frames(){return frames;}
static unsigned e5_input_presented_rgb_pixels(){return rgb;}
static unsigned e5_input_presented_ink_pixels(){return ink;}
static bool e5_input_capture_busy(){return busy;}
static bool e5_input_request_capture(unsigned){busy=true;return true;}
template<class... T> static void note(const char *,T...){nowMs+=1000;}
struct Harness {
 unsigned touchTestKind=4,touchTestStep=0,touchTestFrameMark=0,touchTestWaits=0;
 unsigned settingsIconX=200,settingsIconY=186,settingsRowY=280,settingsMinRgb=20000,settingsMinInk=3000,settingsWaitSeconds=300;
 unsigned settingsGesturePhase=0,settingsCapturedGate=~0u,failures=0,held=0;
 uint64_t touchTestDeadline=0,settingsGestureAt=0;
 const char *settingsGestureAction="none";
 bool touchTestDone=false,touchTestStarted=false,testDown=false;
 E5Input::TouchState touches;
 bool emitTouch(const E5Input::Frame &frame,const char*){events.push_back(nowMs);down=frame.count!=0;touches.previous=frame;return true;}
 void sendKeys(unsigned,const char*){events.push_back(nowMs);down=testDown;}
 void step(){
'''
suffix = r'''
 }
};
int main(){
 Harness h;
 if(!EXPECT_OLD){h.step();assert(!down && busy && events.empty());nowMs+=1000;busy=false;}
 h.step();assert(down && events.size()==1);
 nowMs=h.touchTestDeadline;h.step();assert(!down && events.size()==2);
 uint64_t heldMs=events[1]-events[0];
 if(EXPECT_OLD){assert(heldMs>500);printf("old held_ms=%llu: logging extended a tap into long-press territory\n",(unsigned long long)heldMs);return 0;}
 assert(heldMs==100);printf("PASS held_ms=%llu with a 1000ms blocking logger\n",(unsigned long long)heldMs);
 Harness waiting;events.clear();busy=true;waiting.step();assert(events.empty() && waiting.touchTestStep==0);
 Harness release;release.touchTestStep=1;release.settingsGestureAt=nowMs-100;down=true;release.step();assert(!down && events.size()==1);
 Harness blank;blank.touchTestStep=2;events.clear();busy=false;ink=200;blank.step();assert(events.empty() && blank.touchTestStep==2);
 puts("PASS capture-before-down exclusion, release despite capture, and blank-content refusal");
}
'''
with tempfile.TemporaryDirectory(prefix='e5-ui-timing-') as temporary:
    folder = Path(temporary)
    cpp = folder / 'timing.cpp'
    cpp.write_text(prefix + body + suffix)
    exe = folder / 'timing'
    subprocess.run(['clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-DEXPECT_OLD=' + str(int(args.expect_old)),
                    '-I' + str(source.parent), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
