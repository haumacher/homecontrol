#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Ed25519 keys and detached signatures of HomeControl (issue #35): the installation key's image
# signatures (spec/api.md "Signed runtime images") and the release key's manifest signatures
# (doc/ota.md "Manifest signature"). E-Home signs images itself; this tool serves the development
# update server, hand-signed test images and the fixtures.
#
#   scripts/sign.py keygen <secret-file>          # a new key pair: <secret-file> (mode 600) and <secret-file>.pub
#   scripts/sign.py public <secret-file>          # prints the public key (64 hex digits) and its fingerprint
#   scripts/sign.py image <secret-file> <image>   # writes <image>.sig: Ed25519 over "homecontrol-image-v1" || SHA-256(image)
#   scripts/sign.py manifest <secret-file> <manifest.json>   # writes manifest.sig beside it: Ed25519 over the exact bytes
#
# Key files hold hex and a line end: the secret file the 32-byte private seed (RFC 8032), the .pub
# file the 32-byte public key. A private key never goes into the repository.
#
# Needs the Python package `cryptography` (Debian/Ubuntu: python3-cryptography) or, failing that,
# PyNaCl (python3-nacl).

import hashlib
import os
import sys

IMAGE_CONTEXT = b'homecontrol-image-v1'


class _Signer:
	"""Ed25519 over whichever library is installed."""

	def __init__(self, seed):
		if len(seed) != 32:
			raise SystemExit('a private key is 32 bytes (64 hex digits)')
		try:
			from cryptography.hazmat.primitives import serialization
			from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
			key = Ed25519PrivateKey.from_private_bytes(seed)
			self._sign = key.sign
			self.public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
			return
		except ImportError:
			pass
		try:
			import nacl.signing
			key = nacl.signing.SigningKey(seed)
			self._sign = lambda message: key.sign(message).signature
			self.public = bytes(key.verify_key)
			return
		except ImportError:
			pass
		raise SystemExit('Ed25519 needs the Python package cryptography (python3-cryptography) or PyNaCl (python3-nacl)')

	def sign(self, message):
		return self._sign(message)


def fingerprint(public):
	"""The first 8 bytes of SHA-256 over the key, hex in groups of four (as the device shows it)."""
	digest = hashlib.sha256(public).hexdigest()[:16]
	return ' '.join(digest[i:i + 4] for i in range(0, 16, 4))


def read_secret(path):
	with open(path) as f:
		text = f.read().strip()
	try:
		return bytes.fromhex(text)
	except ValueError:
		raise SystemExit('%s does not hold a private key in hex' % path)


def signer(path):
	return _Signer(read_secret(path))


def keygen(path):
	"""A new key pair: the seed into `path` (mode 600, refusing to overwrite), the public key into
	`path`.pub. Returns the public key."""
	seed = os.urandom(32)
	public = _Signer(seed).public
	fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
	with os.fdopen(fd, 'w') as f:
		f.write(seed.hex() + '\n')
	with open(path + '.pub', 'w') as f:
		f.write(public.hex() + '\n')
	return public


def image_message(image):
	"""The signed message of a runtime image: the context and the SHA-256 of its exact bytes."""
	return IMAGE_CONTEXT + hashlib.sha256(image).digest()


def sign_image(key, image):
	return key.sign(image_message(image))


def sign_manifest(key, manifest):
	return key.sign(manifest)


def main(argv):
	if len(argv) == 3 and argv[1] == 'keygen':
		public = keygen(argv[2])
		print('%s (fingerprint %s) written to %s.pub' % (public.hex(), fingerprint(public), argv[2]))
		return 0
	if len(argv) == 3 and argv[1] == 'public':
		public = signer(argv[2]).public
		print('%s  %s' % (public.hex(), fingerprint(public)))
		return 0
	if len(argv) == 4 and argv[1] in ('image', 'manifest'):
		key = signer(argv[2])
		with open(argv[3], 'rb') as f:
			data = f.read()
		if argv[1] == 'image':
			signature = sign_image(key, data)
			out = argv[3] + '.sig'
		else:
			signature = sign_manifest(key, data)
			out = os.path.join(os.path.dirname(os.path.abspath(argv[3])), 'manifest.sig')
		with open(out, 'w') as f:
			f.write(signature.hex() + '\n')
		print('%s signed with %s: %s' % (argv[3], fingerprint(key.public), out))
		return 0
	print('usage: sign.py keygen <secret-file> | public <secret-file> | image <secret-file> <image> | '
		  'manifest <secret-file> <manifest.json>', file=sys.stderr)
	return 2


if __name__ == '__main__':
	sys.exit(main(sys.argv))
