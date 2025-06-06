#!/usr/bin/env python3

import sys
import subprocess
import time
import os
import signal

proc = subprocess.Popen(sys.argv[1:])

try:
	for _ in range(3):
		if proc.poll() is not None:
			sys.exit(proc.returncode)
		time.sleep(1)
	
	print("Killing process", proc.pid)
	os.kill(proc.pid, signal.SIGABRT)
	
	print("Killed, waiting")
	proc.wait()

except KeyboardInterrupt:
	print("Interrupted.")
	proc.send_signal(signal.SIGABRT)
	proc.wait()

