#!/usr/bin/env python3

import subprocess
from pathlib import Path

from dotenv import load_dotenv

host_ip = "192.168.64.1"
tilde = os.getenv("CODEDIR") 

try:
	mounts = subprocess.run(["mount"], capture_output=True, text=True, check=True)
	mounts = mounts.stdout
except Exception as e:
	print(f"Can't find mounts: {e}")

def is_mounted(local):
	return local in mounts

def mount_nfs(remote, local):
	print(f"Mounting {local}")
	
	try:
		subprocess.run([
			"mount", "-v", "-t" "nfs", f"{host_ip}:/{tilde}/{remote}", 
			local], check=True, capture_output=True)
		#print(f"Mounted in {local}")
		return (True, "")
	except Exception as e:
		return (False, f"Failed to mount {local}: {e}")

def md_config(file, unit=10):
	try:
		result = subprocess.run([
				"mdconfig", "-a", "-t", "vnode", 
				"-f", file, 
				"-u", str(unit)
			], check=True, text=True)
		return (True, "")
	except Exception as e:
		return (False, f"mdconfig failed: {e}")

def list_dmg():
	dmg_files = []
	
	for file in Path("/").glob("*.dmg"):
		try:
			mtime = file.stat().st_mtime
			dmg_files.append((file, mtime))
		except FileNotFoundError:
			continue
	dmg_files.sort(key=lambda x: x[1], reverse=True)
	
	return [str(file) for file, _ in dmg_files]

def list_md(all_md={}):
	try:
		output = subprocess.check_output(
			["geom", "md", "list"], 
			text=True
		)
	except Exception as e:
		print(f"geom md list error: {e}")
	
	units = [9]

	for line in output.splitlines():
		if 'unit:' in line:
			units.append(int(line.strip().split('unit:')[-1].strip()))

	return max(units) + 1

def do_mount(mnt_tup):
	r, l = mnt_tup
	if not is_mounted(l):
		return mount_nfs(r, l)
	else:
		return (False, f"{l} already mounted.")

if __name__ == "__main__":	
	md_list = {}

	steps = [
		('Mounting kernel source',do_mount , (('code/operatingsys/freebsd', '/usr/src'))),
		('Mounting KMOD source', do_mount, (('code/operatingsys/freebsd_hfs', '/sharedcode'))),
		('Loading md device', md_config, (list_md(), list_dmg()[0]))
	]

	for idx, (label, func, args)  in enumerate(steps):
		print(f"[{idx}/len(steps)] {label} ...")
		
		res, txt = func(*args)

		if res:
			print(f"[{idx}/len(steps)] Done!")
		else:
			print(f"[{idx}/len(steps)] {txt}") 
