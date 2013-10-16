Pulse Audio Utilities
========

Pulseaudio is a modern linux audio subsystem.  It is backed
by Ubuntu, and offers an intermediate abstraction between
audio devices and their legacy drivers, and a wide range
of applications.

There are capable GUI interfaces to Pulseaudio, allowing application
streams to be manipulated with a high degree of precision.  However,
existing command line tools don't provide full access to all of these
capabilities.

This repository provides some additional pulse audio command line
tools for manipulating and monitoring your audio.

* *pastat* A streaming status output of the volume of your various streams.
* *patogglepid* Set the output audio sink for streams originating from a given pid.
