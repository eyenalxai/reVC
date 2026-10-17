#!/usr/bin/env python3
# An independent reader of a raw IFP file, for tools/ifp/self_test.sh.
#
# It re-derives the loader's view of every key straight from the bytes:
# CQuaternion::Invert (negate x, y, z), the absolute key times and the
# translations, with no line shared with the C++ tool. It then compares that
# view key for key with what build/ifp dump --keys printed for the same file,
# so the tool's conjugation, its times and its translations are checked against
# something that did not write them.
#
#   build/ifp dump "<gameRoot>/anim/ped.ifp" --keys > /tmp/ped.dump
#   tools/ifp/verify_keys.py "<gameRoot>/anim/ped.ifp" /tmp/ped.dump
#
# Prints the number of keys, the mismatches and a RESULT line; exits 1 on any
# mismatch.

import re
import struct
import sys

PER = {'KR00': 20, 'KRT0': 32, 'KRTS': 44}
TOFF = {'KR00': 16, 'KRT0': 28, 'KRTS': 40}


def ReadRaw(path):
	raw = open(path, 'rb').read()

	def u32(o):
		return struct.unpack_from('<I', raw, o)[0]

	def i32(o):
		return struct.unpack_from('<i', raw, o)[0]

	def rd4(x):
		return (x + 3) & ~3

	o = 8
	count = i32(o + 8)				# the INFO body starts with the animation count
	o += 8 + rd4(u32(o + 4))			# block INFO
	anims = {}
	for _ in range(count):
		size = u32(o + 4)
		name = raw[o + 8:o + 8 + rd4(size)].split(b'\0')[0].decode()
		o += 8 + rd4(size)			# NAME
		o += 8					# DGAN
		numSequences = i32(o + 8)
		o += 8 + rd4(u32(o + 4))		# DGAN INFO
		sequences = {}
		for _ in range(numSequences):
			o += 8				# CPAN
			animSize = u32(o + 4)
			body = raw[o + 8:o + 8 + rd4(animSize)]
			o += 8 + rd4(animSize)		# ANIM
			seq = body[:24].split(b'\0')[0].decode()
			numFrames = struct.unpack_from('<i', body, 28)[0]
			keys = []
			if numFrames:
				ident = raw[o:o + 4].decode()
				per = PER[ident]
				base = o + 8
				for k in range(numFrames):
					rot = struct.unpack_from('<4f', raw, base + k * per)
					rot = (-rot[0], -rot[1], -rot[2], rot[3])
					pos = None
					if per > 20:
						pos = struct.unpack_from('<3f', raw, base + k * per + 16)
					t = struct.unpack_from('<f', raw, base + k * per + TOFF[ident])[0]
					keys.append((rot, pos, t))
				o += 8 + rd4(u32(o + 4))	# KR*
			sequences[seq] = keys
		anims[name] = sequences
	return anims


def ReadDump(path):
	anim = None
	seq = None
	keys = []
	for line in open(path).read().splitlines():
		m = re.match(r'^  (\S.*?): \d+ sequences', line)
		if m:
			anim = m.group(1)
			seq = None
			continue
		m = re.match(r'^    (\S.*?) +tag=.*? frames=(\d+) +keys=(\S+)', line)
		if m:
			seq = m.group(1)
			continue
		m = re.match(r'^      \[(\d+)\] rot=\(([^)]+)\) t=(\S+)'
		             r'(?: pos=\(([^)]+)\))?(?: scale=\(([^)]+)\))?', line)
		if m and anim is not None and seq is not None:
			rot = [float(x) for x in m.group(2).split()]
			t = float(m.group(3))
			pos = [float(x) for x in m.group(4).split()] if m.group(4) else None
			keys.append((anim, seq, int(m.group(1)), rot, pos, t))
	return keys


def main():
	if len(sys.argv) != 3:
		print("usage: verify_keys.py <file.ifp> <dump>", file=sys.stderr)
		return 1
	model = ReadRaw(sys.argv[1])
	keys = ReadDump(sys.argv[2])

	def close(a, b):
		return abs(a - b) <= 1e-5 * max(1.0, abs(b))

	mismatches = []
	for anim, seq, k, rot, pos, t in keys:
		rawRot, rawPos, rawTime = model[anim][seq][k]
		for a, b in zip(rot, rawRot):
			if not close(a, b):
				mismatches.append((anim, seq, k, 'rotation', a, b))
		if not close(t, rawTime):
			mismatches.append((anim, seq, k, 'time', t, rawTime))
		if rawPos is not None:
			for a, b in zip(pos, rawPos):
				if not close(a, b):
					mismatches.append((anim, seq, k, 'translation', a, b))
	for m in mismatches[:10]:
		print("MISMATCH", m)
	print("RESULT: %d keys match the raw file under the loader's conjugation (%d mismatches)"
		% (len(keys), len(mismatches)))
	return 1 if mismatches or not keys else 0


if __name__ == '__main__':
	sys.exit(main())
