# piscope
A digital waveform viewer

piscope uses the services of the pigpio library.  pigpio needs to be running on the Pi whose gpios are to be monitored.

The pigpio library may be started as a daemon (background process) by the following command.

sudo pigpiod

INVOCATION
==========

piscope may be invoked in three different ways

On the Pi
---------

pi_host ~ $ piscope

Pi captures data, Pi processes data, Pi displays data

On the Pi (with the display on a remote machine)
------------------------------------------------

remote_host ~ $ ssh -X pi_host

pi_host ~ $ piscope

Pi captures data, Pi processes data, remote displays data

On a remote machine
-------------------

remote_host ~ $ export PIGPIO_ADDR=pi_host

remote_host ~ $ piscope

Pi captures data, remote processes data, remote displays data

OPERATING MODES
===============

piscope operates in one of three modes

Live
----

The latest gpio samples are displayed.

The mode will automatically change to Pause if a sampling trigger is detected.

There are four triggers.  Each trigger is made up of a combination of gpio states (one of don't care, low, high, edge, falling, or rising per gpio).  Triggers are always counted.  In addition a trigger may be sample to, sample around, or sample from, a so called sampling trigger.

New samples are added to the sample buffer.

Once the sample buffer is full the oldest samples are discarded.

Play
----

Recorded gpio samples are displayed.

The play speed may be varied between 64 times real-time to 1/32768 of real-time.

The page up key increases the play speed by a factor of 2.  The page down key decreases the play speed by a factor of 2.  The home key sets the play speed to 1X.

New samples are added to the sample buffer.

Once the sample buffer is full new samples are discarded.

Pause
-----

Recorded gpio samples are displayed.

The left and right cursor keys move the blue marker to the previous or next edge.  By default all gpio edges are considered.  Clicking on a gpio name will limit edge searches to the highlighted gpios only.

The left and right square bracket keys move the blue marker to the previous or next trigger.

The time between the blue and gold markers is displayed.  The gold marker is set to the blue marker by a press of the 'g' key.

New samples are added to the sample buffer.

Once the sample buffer is full new samples are discarded.

NOTES
=====

In all modes the down and up cursor keys zoom the time scale in and out.

Samples can be saved with File Save All Samples or File Save Selected Samples.

To select samples enter pause mode.  Press 1 to specify the start of the samples (green marker) and 2 to specify the end of the samples (red marker).

The samples may be saved in the native piscope format or in VCD format.

Data saved in VCD format may be viewed and further processed with GTKWave.

Data saved in the native piscope format may be restored later with File Restore Saved Data.

