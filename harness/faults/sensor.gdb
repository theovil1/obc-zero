# Make a sensor lie, in each of the ways ADR 0008 says are detectable.
#
# $sensor_index : which sensor, 0-based
# $sensor_mode  : 0 honest, 1 out of range, 2 stuck on a plausible value
# $sensor_runs  : how many samples to drive before letting the run finish
#
# The mode is a parameter and not three scripts, because the three failures share
# one thing that must be identical between them: the value is written into the
# same backend word at the same point in the dispatch. Three scripts would drift
# apart at exactly the place where a difference would invalidate the comparison.
#
# **The bounds are read from the binary, never restated here.** obc_sensor_desc
# is the same table the validator compares against, so "just outside the range"
# means just outside the range the vehicle is actually applying. An injector
# holding its own copy of the bounds would keep passing after the flight bounds
# changed, and would be testing its own opinion.
#
# The honest and out-of-range modes both *vary* their value between samples. A
# constant would set the stuck bit as well, and a case that trips two detectors
# proves neither of them: it cannot distinguish a validator that caught the
# range from one that caught the repetition.
#
# Copyright 2026 Théo Vilain
# SPDX-License-Identifier: Apache-2.0

set confirm off
set pagination off

set architecture riscv:rv32

if $_isvoid($campaign_port)
  set $campaign_port = 1234
end
eval "target remote localhost:%d", $campaign_port

if $_isvoid($sensor_index)
  set $sensor_index = 0
end
if $_isvoid($sensor_mode)
  set $sensor_mode = 1
end
# Must not exceed the number of times telemetry is actually dispatched in the
# window, or the last `continue` waits for a breakpoint hit that will never come
# and the run dies on the harness timeout — a red that names the injector rather
# than anything under test.
if $_isvoid($sensor_runs)
  set $sensor_runs = 8
end

set $lo = (unsigned int)obc_sensor_desc[$sensor_index].min
set $hi = (unsigned int)obc_sensor_desc[$sensor_index].max
set $span = $hi - $lo

printf "SENSOR-INJECT index=%d mode=%d range=[%u,%u] runs=%d\n", \
  $sensor_index, $sensor_mode, $lo, $hi, $sensor_runs

# Written on entry to the sampler, so the value the validator reads is the value
# this wrote and nothing ran in between. Writing it once before the window would
# test one sample and let the remaining ones read whatever the poison left.
break obc_sensor_sample

set $n = 0
while $n < $sensor_runs
  continue

  if $sensor_mode == 0
    # In range, and different every time: the case that must produce a frame
    # with no flag set at all. This is the direction that stops the flag
    # becoming decorative — a validator that flags everything fails here.
    set $v = $lo + (($n * 37) % $span)
  end
  if $sensor_mode == 1
    # Above the declared maximum, varying, so only the range bit can be the
    # reason it was caught.
    set $v = $hi + 1 + ($n % 3)
  end
  if $sensor_mode == 2
    # Perfectly plausible and never changing. Nothing about this value is wrong;
    # what is wrong is that it is the same one. The only detector that can see
    # it is the run length.
    set $v = $lo + ($span / 2)
  end

  set obc_sensor_mock_raw[$sensor_index] = (unsigned short)$v
  printf "SENSOR-SET n=%d value=%u\n", $n, $v
  set $n = $n + 1
end

delete
detach
quit
